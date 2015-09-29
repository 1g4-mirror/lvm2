/*
 * Copyright (C) 2011-2015 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "archiver.h"
#include "metadata.h"
#include "toolcontext.h"
#include "segtype.h"
#include "display.h"
#include "activate.h"
#include "lv_alloc.h"
#include "lvm-string.h"
#include "lvm-signal.h"

#define	ARRAY_SIZE(a) (sizeof(a) / sizeof(*a))

/* Ensure minimum region size on @lv */
static void _ensure_min_region_size(struct logical_volume *lv)
{
	struct lv_segment *seg = first_seg(lv);
	/* MD's bitmap is limited to tracking 2^21 regions */
	uint32_t min_region_size = lv->size / (1 << 21);
	uint32_t region_size = seg->region_size;

	while (seg->region_size < min_region_size)
		seg->region_size *= 2;

	if (seg->region_size != region_size)
		log_very_verbose("Setting region_size to %u", seg->region_size);
}

/* HM Return "linear" for striped @segtype with 1 area instead of "striped" */
static const char *_get_segtype_name(const struct segment_type *segtype, unsigned new_image_count)
{
	if (!segtype)
		return "linear";

	return (segtype_is_striped(segtype) && new_image_count == 1) ? "linear" : segtype->name;
}

/*
 * HM
 *
 * Default region_size on @lv unless already set i
 */
static void _check_and_init_region_size(struct logical_volume *lv)
{
	struct lv_segment *seg = first_seg(lv);

	seg->region_size = seg->region_size ?: get_default_region_size(lv->vg->cmd);
	_ensure_min_region_size(lv);
}

/* Return data images count for @total_rimages depending on @seg's type */
static uint32_t _data_rimages_count(const struct lv_segment *seg, const uint32_t total_rimages)
{
	return total_rimages - seg->segtype->parity_devs;
}

/*
 * HM
 *
 * Compare the raid levels in segtype @t1 and @t2
 *
 * Return 1 if same, else 0
 */
static int _cmp_level(const struct segment_type *t1, const struct segment_type *t2)
{
	/* raid10 */
	if (!strncmp(t1->name, t2->name, 6))
		return 1;

	return !strncmp(t1->name, t2->name, 5);
}

/*
 * HM
 *
 * Check for same raid levels in segtype @t1 and @t2
 *
 * Return 1 if same, else != 1
 */
static int is_same_level(const struct segment_type *t1, const struct segment_type *t2)
{
#if 0
	static uint64_t level_flags[] = {
		SEG_RAID0|SEG_RAID0_META,
		SEG_RAID5_LS|SEG_RAID5_LA|SEG_RAID5_RS|SEG_RAID5_RA|SEG_RAID5_N,
		SEG_RAID6_ZR|SEG_RAID6_NC|SEG_RAID6_NR| \
		SEG_RAID6_LS_6|SEG_RAID6_LA_6|SEG_RAID6_RS_6|SEG_RAID6_RA_6|SEG_RAID6_N_6
	};
	unsigned i = ARRAY_SIZE(level_flags);

	if (t1->flag & t2->flag)
		return 1;

	while (i--)
		if ((t1->flag & level_flags[i]) && (t2->flag & level_flags[i]))
			return 1;

	return 0;
#else
	return _cmp_level(t1, t2);
#endif
}

static int _lv_is_raid_with_tracking(const struct logical_volume *lv,
				     struct logical_volume **tracking)
{
	uint32_t s;
	const struct lv_segment *seg = first_seg(lv);

	*tracking = NULL;

	if (!lv_is_raid(lv))
		return 0;

	for (s = 0; s < seg->area_count; s++)
		 if (lv_is_visible(seg_lv(seg, s)) &&
		     !(seg_lv(seg, s)->status & LVM_WRITE)) {
			*tracking = seg_lv(seg, s);
			return 1;
		}

	return 0;
}

int lv_is_raid_with_tracking(const struct logical_volume *lv)
{
	struct logical_volume *tracking;

	return _lv_is_raid_with_tracking(lv, &tracking);
}

/* Helper: return true in case this is a raid1 top-level lv inserted to do synchronization of 2 given sublvs */
static int _lv_is_duplicating(const struct logical_volume *lv)
{
	struct lv_segment *seg = first_seg(lv);

	/* Needs to be raid1 with 2 legs and the legs must have the proper names suffixes */
	return (seg_is_raid1(seg) &&
		seg->area_count == 2 &&
		strstr(seg_lv(seg, 0)->name, "_dsrc") &&
		strstr(seg_lv(seg, 1)->name, "_ddst"));
}

uint32_t lv_raid_image_count(const struct logical_volume *lv)
{
	struct lv_segment *seg = first_seg(lv);

	return seg_is_raid(seg) ? seg->area_count : 1;
}

/* Calculate raid rimage length based on total @extents for segment and @stripes and @data_copies */
uint32_t lv_raid_rimage_extents(uint32_t extents, uint32_t stripes, uint32_t data_copies)
{
	uint64_t r = extents;

	r *= data_copies ?: 1;
	r = dm_div_up(r, stripes ?: 1);

	return r > UINT_MAX ? 0 : (uint32_t) r;
}

static int _activate_sublv_preserving_excl(struct logical_volume *top_lv,
					   struct logical_volume *sub_lv)
{
	struct cmd_context *cmd = top_lv->vg->cmd;

	/* If top RAID was EX, use EX */
	if (lv_is_active_exclusive_locally(top_lv)) {
		if (!activate_lv_excl_local(cmd, sub_lv))
			return_0;
	} else if (!activate_lv(cmd, sub_lv))
			return_0;

	return 1;
}

/* Correct segments start logical extents in all sub LVs of @lv */
static void _lv_set_image_lvs_start_les(struct logical_volume *lv)
{
	uint32_t s;
	struct lv_segment *raid_seg = first_seg(lv);

	for (s = 0; s < raid_seg->area_count; s++) {
		uint32_t le = 0;
		struct logical_volume *slv = seg_lv(raid_seg, s);
		struct lv_segment *seg;

		dm_list_iterate_items(seg, &slv->segments) {
			seg->le = le;
			le += seg->len;
		}
	}
}

/*
 * HM
 *
 * Put @lv on @removal_lvs resetting it's raid image state
 */
static int _lv_reset_raid_add_to_list(struct logical_volume *lv, struct dm_list *removal_lvs)
{
	struct lv_list *lvl;

	if (!(lvl = dm_pool_alloc(lv->vg->vgmem, sizeof(*lvl)))) {
		log_error(INTERNAL_ERROR "Failed to allocate lv lsst item");
		return 0;
	}

	lvl->lv = lv;
	lv->status &= ~(RAID | RAID_IMAGE);
	lv_set_visible(lv);
	dm_list_add(removal_lvs, &lvl->list);

	return 1;
}

/*
 * HM
 *
 * Deactivate and remove the LVs on @removal_lvs list from @vg
 *
 * Returns 1 on success or 0 on failure
 */
static int _deactivate_and_remove_lvs(struct volume_group *vg, struct dm_list *removal_lvs)
{
	struct lv_list *lvl;

	dm_list_iterate_items(lvl, removal_lvs) {
PFLA("lvl->lv->name=%s", lvl->lv->name);
		if (!deactivate_lv(vg->cmd, lvl->lv))
			return_0;
PFL();
		if (!lv_remove(lvl->lv))
			return_0;
PFL();
	}

	return 1;
}

/*
 * _raid_in_sync
 * @lv
 *
 * _raid_in_sync works for all types of RAID segtypes, as well
 * as 'mirror' segtype.  (This is because 'lv_raid_percent' is
 * simply a wrapper around 'lv_mirror_percent'.
 *
 * Returns: 1 if in-sync, 0 otherwise.
 */
static int _raid_in_sync(struct logical_volume *lv)
{
	dm_percent_t sync_percent;
	struct lv_segment *seg = first_seg(lv);

	if (seg_is_striped(seg) || seg_is_any_raid0(seg))
		return 1;

	if (!lv_raid_percent(lv, &sync_percent)) {
		log_error("Unable to determine sync status of %s.", display_lvname(lv));
		return 0;
	}
PFLA("sync_percent=%d DM_PERCENT_100=%d", sync_percent, DM_PERCENT_100);
	if (sync_percent == DM_PERCENT_0) {
		/*
		 * FIXME We repeat the status read here to workaround an
		 * unresolved kernel bug when we see 0 even though the 
		 * the array is 100% in sync.
		 * https://bugzilla.redhat.com/1210637
		 */
		if (!lv_raid_percent(lv, &sync_percent)) {
			log_error("Unable to determine sync status of %s/%s.",
				  lv->vg->name, lv->name);
			return 0;
		}
PFLA("sync_percent=%d DM_PERCENT_100=%d", sync_percent, DM_PERCENT_100);
		if (sync_percent == DM_PERCENT_100)
			log_warn("WARNING: Sync status for %s is inconsistent.",
				 display_lvname(lv));
	}

	return (sync_percent == DM_PERCENT_100) ? 1 : 0;
}

/* Start repair on idle/frozen @lv */
static int _lv_cond_repair(struct logical_volume *lv)
{
	char *action;

	if (!lv_raid_sync_action(lv, &action))
		return 0;

	return (strcmp(action, "idle") &&
		strcmp(action, "frozen")) ? 1 : lv_raid_message(lv, "repair");
}

/*
 * Report current number of redundant disks for @total_images and @segtype 
 */
static void _seg_get_redundancy(const struct segment_type *segtype, unsigned total_images, unsigned *nr)
{
	if (!segtype)
		*nr = 0;

	else if (segtype_is_raid10_near(segtype))
		*nr = total_images / 2; /* Only if one in each stripe failing */

	else if (segtype_is_any_raid10(segtype))
		*nr = 1; /* HM FIXME: correct? */

	else if (segtype_is_raid1(segtype))
		*nr = total_images - 1;

	else if (segtype_is_raid4(segtype) ||
		 segtype_is_any_raid5(segtype) ||
		 segtype_is_any_raid6(segtype))
		*nr = segtype->parity_devs;

	else
		*nr = 0;
}

/*
 * In case of any resilience related conversions on @lv -> ask the user unless "-y/--yes" on command line
 */
static int _yes_no_conversion(const struct logical_volume *lv,
			      const struct segment_type *new_segtype,
			      int yes, int force,
			      unsigned new_image_count,
			      const unsigned new_stripes,
			      const unsigned new_stripe_size)
{
	int segtype_change, stripes_change, stripe_size_change;
	unsigned cur_redundancy, new_redundancy;
	struct lv_segment *seg = first_seg(lv);
	struct segment_type *new_segtype_tmp = (struct segment_type *) new_segtype;
	const struct segment_type *segtype;
	struct lvinfo info = { 0 };

	if (!lv_info(lv->vg->cmd, lv, 0, &info, 1, 0) && driver_version(NULL, 0)) {
		log_error("Unable to retrieve logical volume information: aborting");
		return 0;
	}

	/* If this is a duplicating lv with raid1 on top, the segtype of the respective leg is relevant */
	if (_lv_is_duplicating(lv)) {
		if (first_seg(seg_lv(seg, 0))->segtype == new_segtype)
			segtype = first_seg(seg_lv(seg, 1))->segtype;
		else
			segtype = first_seg(seg_lv(seg, 0))->segtype;

	} else
		segtype = seg->segtype;

	segtype_change = new_segtype != segtype;
	stripes_change = new_stripes && (new_stripes != _data_rimages_count(seg, seg->area_count));
	stripe_size_change = new_stripe_size && (new_stripe_size != seg->stripe_size);

	new_image_count = new_image_count ?: lv_raid_image_count(lv);

	/* Get number of redundant disk for current and new segtype */
	_seg_get_redundancy(segtype, seg->area_count, &cur_redundancy);
	_seg_get_redundancy(new_segtype, new_image_count ?: lv_raid_image_count(lv), &new_redundancy);

PFLA("yes=%d cur_redundancy=%u new_redundancy=%u", yes, cur_redundancy, new_redundancy);
	if (new_redundancy == cur_redundancy) {
		if (stripes_change)
			log_print_unless_silent("Converting active%s %s %s%s%s%s will keep "
				 "resilience of %u disk failure%s",
				 info.open_count ? " and open" : "", display_lvname(lv),
				 segtype != new_segtype ? "from " : "",
				 segtype != new_segtype ? _get_segtype_name(segtype, seg->area_count) : "",
				 segtype != new_segtype ? " to " : "",
				 segtype != new_segtype ? _get_segtype_name(new_segtype, new_image_count) : "",
				 cur_redundancy,
				 (!cur_redundancy || cur_redundancy > 1) ? "s" : "");

	} else if (new_redundancy > cur_redundancy)
		log_print_unless_silent("Converting active%s %s %s%s%s%s will enhance "
			 "resilience from %u disk failure%s to %u",
			 info.open_count ? " and open" : "", display_lvname(lv),
			 segtype != new_segtype ? "from " : "",
			 segtype != new_segtype ? _get_segtype_name(segtype, seg->area_count) : "",
			 segtype != new_segtype ? " to " : "",
			 segtype != new_segtype ? _get_segtype_name(new_segtype, new_image_count) : "",
			 cur_redundancy,
			 (!cur_redundancy || cur_redundancy > 1) ? "s" : "",
			 new_redundancy);

	else if (new_redundancy &&
		 new_redundancy < cur_redundancy)
		log_warn("WARNING: Converting active%s %s %s%s%s%s will degrade "
			 "resilience from %u disk failures to just %u",
			 info.open_count ? " and open" : "", display_lvname(lv),
			 segtype != new_segtype ? "from " : "",
			 segtype != new_segtype ? _get_segtype_name(segtype, seg->area_count) : "",
			 segtype != new_segtype ? " to " : "",
			 segtype != new_segtype ? _get_segtype_name(new_segtype, new_image_count) : "",
			 cur_redundancy, new_redundancy);

	else if (!new_redundancy && cur_redundancy)
		log_warn("WARNING: Converting active%s %s from %s to %s will degrade "
			 "all resilience to %u disk failure%s",
			 info.open_count ? " and open" : "", display_lvname(lv),
			 _get_segtype_name(segtype, seg->area_count),
			 _get_segtype_name(new_segtype, new_image_count),
			 cur_redundancy, cur_redundancy > 1 ? "s" : "");


	/****************************************************************************/
	/* No --type arg */
	/* Linear/raid0 with 1 image to raid1 via "-mN" option */
	if (segtype == new_segtype &&
	    (seg_is_linear(seg) || (seg_is_any_raid0(seg) && seg->area_count == 1)) &&
    	    new_image_count > 1 &&
	    !(new_segtype_tmp = get_segtype_from_flag(lv->vg->cmd, SEG_RAID1)))
		return_0;

	if (!yes) {
		if (segtype_change &&
		    yes_no_prompt("Do you really want to convert %s with type %s to %s? [y/n]: ",
				display_lvname(lv),
				_get_segtype_name(segtype, seg->area_count),
				_get_segtype_name(new_segtype_tmp, new_image_count)) == 'n') {
			log_error("Logical volume %s NOT converted", display_lvname(lv));
			return 0;
		}

		if (stripes_change &&
		    yes_no_prompt("Do you really want to convert %s from %u stripes to %u stripes? [y/n]: ",
				display_lvname(lv), _data_rimages_count(seg, seg->area_count), new_stripes) == 'n') {
			log_error("Logical volume %s NOT converted", display_lvname(lv));
			return 0;
		}

		if (stripe_size_change &&
		    yes_no_prompt("Do you really want to convert %s from stripesize %d to stripesize %d? [y/n]: ",
				display_lvname(lv),
				seg->stripe_size, new_stripe_size) == 'n') {
			log_error("Logical volume %s NOT converted", display_lvname(lv));
			return 0;
		}
	}
	if (sigint_caught())
		return_0;

	/* Now archive metadata after the user has confirmed */
	if (!archive(lv->vg))
		return_0;

	return 1;
}

/* HM */
static void _avoid_pv_of_lv(struct logical_volume *lv, struct physical_volume *pv)
{
	if (!(lv->status & PARTIAL_LV) &&
	    lv_is_on_pv(lv, pv))
		pv->status |= PV_ALLOCATION_PROHIBITED;
}

static int _avoid_pvs_of_lv(struct logical_volume *lv, void *data)
{
	struct dm_list *allocate_pvs = (struct dm_list *) data;
	struct pv_list *pvl;

	dm_list_iterate_items(pvl, allocate_pvs)
		_avoid_pv_of_lv(lv, pvl->pv);

	return 1;
}

/*
 * Prevent any PVs holding other image components of @lv from being used for allocation,
 * I.e. remove respective PVs from @allocatable_pvs
 */
static int _avoid_pvs_with_other_images_of_lv(struct logical_volume *lv, struct dm_list *allocate_pvs)
{
	if (seg_type(first_seg(lv), 0) == AREA_PV)
		_avoid_pvs_of_lv(lv, allocate_pvs);
	else
		for_each_sub_lv(lv, _avoid_pvs_of_lv, allocate_pvs);

	return 1;
}

/*
 * _convert_raid_to_linear
 * @lv
 * @removal_lvs
 *
 * Remove top layer of RAID LV in order to convert to linear.
 * This function makes no on-disk changes.  The residual LVs
 * returned in 'removal_lvs' must be freed by the caller.
 *
 * Returns: 1 on succes, 0 on failure
 */
static int _extract_image_component_list(struct lv_segment *seg,
					 uint64_t type, uint32_t idx,
					 struct dm_list *removal_lvs);
static int _convert_raid_to_linear(struct logical_volume *lv,
				   struct dm_list *removal_lvs)
{
	struct logical_volume *lv_tmp;
	struct lv_segment *seg = first_seg(lv);

	if (!seg_is_any_raid0(seg) &&
	    !seg_is_mirrored(seg) && !seg_is_raid1(seg) &&
	    !seg_is_raid4(seg) && !seg_is_any_raid5(seg)) {
		log_error(INTERNAL_ERROR
			  "Unable to remove RAID layer from segment type %s",
			  lvseg_name(seg));
		return 0;
	}

	/* Only one area may result from the check! */
	if (seg->area_count != 1) {
		log_error(INTERNAL_ERROR
			  "Unable to remove RAID layer when there"
			  " is more than one sub-lv");
		return 0;
	}

	if (seg->meta_areas &&
	    !_extract_image_component_list(seg, RAID_META, 0 /* idx */, removal_lvs))
		return 0;

	/* Add remaining last image lv to removal_lvs */
	lv_tmp = seg_lv(seg, 0);
	if (!_lv_reset_raid_add_to_list(lv_tmp, removal_lvs))
		return 0;

	if (!remove_layer_from_lv(lv, lv_tmp))
		return_0;

	if (!(first_seg(lv)->segtype = get_segtype_from_string(lv->vg->cmd, SEG_TYPE_NAME_STRIPED)))
		return_0;

	lv->status &= ~(MIRRORED | RAID);

	return 1;
}

/*
 * _clear_lv
 * @lv
 *
 * If LV is active:
 *        clear first block of device
 * otherwise:
 *        activate, clear, deactivate
 *
 * Returns: 1 on success, 0 on failure
 */
static int _clear_lv(struct logical_volume *lv)
{
	int was_active = lv_is_active_locally(lv);

	if (test_mode())
		return 1;

	lv->status |= LV_TEMPORARY;
	if (!was_active && !activate_lv_local(lv->vg->cmd, lv)) {
		log_error("Failed to activate localy %s for clearing",
			  display_lvname(lv));
		return 0;
	}
	lv->status &= ~LV_TEMPORARY;

PFLA("Clearing metadata area of %s", display_lvname(lv));
	log_verbose("Clearing metadata area of %s", display_lvname(lv));
	/*
	 * Rather than wiping lv->size, we can simply
	 * wipe the first sector to remove the superblock of any previous
	 * RAID devices.  It is much quicker.
	 */
	if (!wipe_lv(lv, (struct wipe_params) { .do_zero = 1, .zero_sectors = 1 })) {
		log_error("Failed to zero %s", display_lvname(lv));
		return 0;
	}

	if (!was_active && !deactivate_lv(lv->vg->cmd, lv)) {
		log_error("Failed to deactivate %s", display_lvname(lv));
		return 0;
	}

	return 1;
}

/*
 * HM
 *
 * Wipe all LVs on @lv_list
 *
 * Makes on-disk metadata changes!
 *
 * Returns 1 on success or 0 on failure
 */
static int _clear_lvs(struct dm_list *lv_list)
{
	struct lv_list *lvl;
	struct volume_group *vg = NULL;

	if (dm_list_empty(lv_list)) {
		log_debug_metadata(INTERNAL_ERROR "Empty list of LVs given for clearing");
		return 1;
	}

	dm_list_iterate_items(lvl, lv_list) {
		if (!lv_is_visible(lvl->lv)) {
			log_error(INTERNAL_ERROR "LVs must be set visible before clearing");
			return 0;
		}

		vg = lvl->lv->vg;
	}

	/*
	 * FIXME: only vg_[write|commit] if LVs are not already written
	 * as visible in the LVM metadata (which is never the case yet).
	 */
PFL();
PFLA("vg_validate(vg)=%d", vg_validate(vg));
PFL();
	if (!vg || !vg_write(vg) || !vg_commit(vg))
		return_0;
PFL();
	dm_list_iterate_items(lvl, lv_list)
		if (!_clear_lv(lvl->lv))
			return 0;

	return 1;
}

/*
 * HM
 *
 * Check for maximum supported raid devices imposed by the kernel MD
 * maximum device limits _and_ dm-raid superblock bitfield constraints
 *
 * Returns 1 on success or 0 on failure
 */
static int _check_max_raid_devices(uint32_t image_count)
{
	if (image_count > DEFAULT_RAID_MAX_IMAGES) {
		log_error("Unable to handle arrays with more than %u devices",
		          DEFAULT_RAID_MAX_IMAGES);
		return 0;
	}

	return 1;
}

/*
 * HM
 *
 * Check for maximum supported mirror devices imposed
 * by the kernel mirror target maximum device
 *
 * Returns 1 on success or 0 on failure
 */
static int _check_max_mirror_devices(uint32_t image_count)
{
	if (image_count > DEFAULT_MIRROR_MAX_IMAGES) {
		log_error("Unable to handle data_copies with more than %u devices",
		          DEFAULT_MIRROR_MAX_IMAGES);
		return 0;
	}

	return 1;
}

/* Replace @lv with error segment */
static int _replace_lv_with_error_segment(struct logical_volume *lv)
{
	if (lv && (lv->status & PARTIAL_LV)) {
		log_debug("Replacing %s segments with error target", lv->name);

		if (!replace_lv_with_error_segment(lv)) {
			log_error("Failed to replace %s's extents with error target.", display_lvname(lv));
			return 0;
		}
	}

	return 1;
}

/* Retrieve index from @*lv_name and add it to @prefix; set the result in @*lv_name */
static int _lv_name_add_string_index(struct cmd_context *cmd, const char **lv_name, const char *prefix)
{
	size_t len;
	char *b, *e, *newname, *tmpname;

	if (!(tmpname = dm_pool_strdup(cmd->mem, *lv_name)))
		return 0;

	if (!(e = strrchr(tmpname, '_')))
		return 0;

	*e = '\0';
	if (!(b = strrchr(tmpname, '_')))
		return 0;

	len = strlen(prefix) + (e - b) + 1;
	if (!(newname = dm_pool_zalloc(cmd->mem, len))) {
		log_error("Failed to allocate new LV name");
		return 0;
	}

	strcpy(newname, prefix);
	strcat(newname, b);
	*lv_name = newname;
	return 1;
}

/*
 * Shift image @*name (suffix) from @s to (@s - @missing)
 *
 * E.g. s=5, missing=2 -> change "*_r(image,meta)_5" to "*_r(image,meta)_3" 
 */
static int _shift_image_name(struct lv_segment *seg, char **name, unsigned s, unsigned missing)
{
	unsigned num;
	size_t len;
	char *numptr, *shift_name;

	if (!(shift_name = dm_pool_strdup(seg_lv(seg, s - missing)->vg->cmd->mem, *name))) {
		log_error("Memory allocation failed.");
		return 0;
	}

	if (!(numptr = strrchr(shift_name, '_')) ||
			(num = atoi(numptr + 1)) != s) {
		log_error("Malformatted image name");
		return 0;
	}

	*name = shift_name;
	len = strlen(++numptr) + 1;

	return dm_snprintf(numptr, len, "%u", num - missing) < 0 ? 0 : 1;
}

/*
 * _shift_image_components
 * @seg: Top-level RAID segment
 *
 * Shift all higher indexed segment areas down to fill in gaps where
 * there are 'AREA_UNASSIGNED' areas.
 *
 * We don't need to bother with name reallocation,
 * because the name length will be less or equal
 * when shifting down as opposed to shifting up.
 *
 * Returns: 1 on success, 0 on failure
 */
static int _shift_image_components(struct lv_segment *seg)
{
	uint32_t s, missing;

	if (!seg_is_raid(seg))
		return_0;

	/* Should not be possible here, but... */
	if (!_check_max_raid_devices(seg->area_count))
		return 0;

	log_very_verbose("Shifting images in %s", lvseg_name(seg));

	for (s = missing = 0; s < seg->area_count; s++) {
		if (seg_type(seg, s) == AREA_UNASSIGNED) {
			if (seg_metatype(seg, s) != AREA_UNASSIGNED) {
				log_error(INTERNAL_ERROR "Metadata segment area"
						" #%d should be AREA_UNASSIGNED", s);
				return 0;
			}

			missing++;
			continue;
		}

		if (missing) {
			log_very_verbose("Shifting %s and %s by %u",
					seg_metalv(seg, s)->name,
					seg_lv(seg, s)->name, missing);
			seg->areas[s - missing] = seg->areas[s];
			if (!_shift_image_name(seg, (char **) &seg_lv(seg, s - missing)->name, s, missing))
				return 0;

			if (seg->meta_areas) {
				seg->meta_areas[s - missing] = seg->meta_areas[s];
				if (!_shift_image_name(seg, (char **) &seg_metalv(seg, s - missing)->name, s, missing))
					return 0;
			}
		}

	}

	seg->area_count -= missing;
	return 1;
}

/* Generate raid subvolume name and validate it */
static char *_generate_raid_name(struct logical_volume *lv,
		const char *suffix, int count)
{
	const char *format = (count < 0) ? "%s_%s" : "%s_%s_%u";
	size_t len = strlen(lv->name) + strlen(suffix) + ((count < 0) ? 2 : 5);
	char *name;

	if (!(name = dm_pool_alloc(lv->vg->vgmem, len))) {
		log_error("Failed to allocate new name.");
		return NULL;
	}

	if (dm_snprintf(name, len, format, lv->name, suffix, count) < 0)
		return_NULL;

PFLA("name=%s", name);
	if (!validate_name(name)) {
		log_error("New logical volume name \"%s\" is not valid.", name);
		return NULL;
	}

	if (find_lv_in_vg(lv->vg, name)) {
		log_error("Logical volume %s already exists in volume group %s.",
				name, lv->vg->name);
		return NULL;
	}

	return name;
}

/*
 * Eliminate the extracted LVs on @removal_lvs from @vg incl. vg write, commit and backup 
 */
static int _eliminate_extracted_lvs(struct volume_group *vg, struct dm_list *removal_lvs)
{
	if (!removal_lvs || dm_list_empty(removal_lvs))
		return 1;

	sync_local_dev_names(vg->cmd);
PFL();
	if (!_deactivate_and_remove_lvs(vg, removal_lvs))
		return 0;

	if (!vg_write(vg) || !vg_commit(vg))
		return_0;

	if (!backup(vg))
		log_error("Backup of VG %s failed after removal of image component LVs", vg->name);
PFL();

	return 1;
}

/*
 * Reallocate segment areas given by @type (i.e. data or metadata areas)
 * in first segment of @lv to amount in @areas copying the minimum of common areas across
 */
static int _realloc_seg_areas(struct logical_volume *lv,
			      uint32_t areas, uint64_t type)
{
	struct lv_segment *seg = first_seg(lv);
	struct lv_segment_area **seg_areas;
	struct lv_segment_area *new_areas;

	switch (type) {
	case RAID_META:
		seg_areas = &seg->meta_areas;
		break;
	case RAID_IMAGE:
		seg_areas = &seg->areas;
		break;
	default:
		log_error(INTERNAL_ERROR "Called with bogus type argument");
		return 0;
	}

	if (!(new_areas = dm_pool_zalloc(lv->vg->vgmem, areas * sizeof(*new_areas)))) {
		log_error("Allocation of new areas array failed.");
		return 0;
	}

	if (*seg_areas)
		memcpy(new_areas, *seg_areas, min(areas, seg->area_count) * sizeof(*new_areas));

	*seg_areas = new_areas;

	return 1;
}

/*
 * HM
 *
 * Reallocate both data and metadata areas of first segment of segment of @lv to new amount in @areas
 */
static int _realloc_meta_and_data_seg_areas(struct logical_volume *lv, uint32_t areas)
{
	return (_realloc_seg_areas(lv, areas, RAID_META) &&
		_realloc_seg_areas(lv, areas, RAID_IMAGE)) ? 1 : 0;
}

/*
 * _extract_image_component
 * @seg
 * @idx:  The index in the areas array to remove
 * @data: != 0 to extract data dev / 0 extract metadata_dev
 * @extracted_lv:  The displaced metadata/data LV
 * @set_error_seg: if set, replace lv of @type at @idx with error segment
 */
static int _extract_image_component_error_seg(struct lv_segment *seg,
					      uint64_t type, uint32_t idx,
					      struct logical_volume **extracted_lv,
					      int set_error_seg)
{
	struct logical_volume *lv;

	switch (type) {
		case RAID_META:
			lv = seg_metalv(seg, idx);
			seg_metalv(seg, idx) = NULL;
			seg_metatype(seg, idx) = AREA_UNASSIGNED;
			break;
		case RAID_IMAGE:
			lv = seg_lv(seg, idx);
			seg_lv(seg, idx) = NULL;
			seg_type(seg, idx) = AREA_UNASSIGNED;
			break;
		default:
			log_error(INTERNAL_ERROR "Bad type provided to %s.", __func__);
			return 0;
	}

	if (!lv)
		return 0;

	log_very_verbose("Extracting image component %s from %s", lv->name, lvseg_name(seg));
	lv->status &= ~(type | RAID);
	lv_set_visible(lv);

	/* release lv areas */
	if (!remove_seg_from_segs_using_this_lv(lv, seg))
		return_0;

	if (!(lv->name = _generate_raid_name(lv, "extracted", -1)))
		return_0;

	if (set_error_seg &&
	    !replace_lv_with_error_segment(lv))
		return_0;

	*extracted_lv = lv;

	return 1;
}

static int _extract_image_component(struct lv_segment *seg,
				    uint64_t type, uint32_t idx,
				    struct logical_volume **extracted_lv,
				    int set_error_seg)
{
	return _extract_image_component_error_seg(seg, type, idx, extracted_lv, set_error_seg);
}

/*
 * @seg
 * @idx:  The index in the areas array to remove
 * @lvl_array: The displaced metadata + data LVs
 *
 * These functions extracts _one_  image component pair - setting the respective
 * 'lvl_array' pointers.  It appends '_extracted' to the LVs' names, so that
 * there are not future conflicts.  It does /not/ commit the results.
 * (IOW, erroring-out requires no unwinding of operations.)
 *
 * This function does /not/ attempt to:
 *    1) shift the 'areas' or 'meta_areas' arrays.
 *       The '[meta_]areas' are left as AREA_UNASSIGNED.
 *    2) Adjust the seg->area_count
 *    3) Name the extracted LVs appropriately (appends '_extracted' to names)
 * These actions must be performed by the caller.
 *
 * Returns: 1 on success, 0 on failure
 */
static int _extract_image_component_pair(struct lv_segment *seg, uint32_t idx,
					 struct lv_list *lvl_array,
					 struct dm_list *extracted_meta_lvs,
					 struct dm_list *extracted_data_lvs,
					 int set_error_seg)
{
	if (idx >= seg->area_count) {
		log_error(INTERNAL_ERROR "area index too large for segment");
		return 0;
	}

	/* Don't change extraction sequence; callers are relying on it */
	if (!_extract_image_component(seg, RAID_META,  idx, &lvl_array[0].lv, set_error_seg) ||
	    !_extract_image_component(seg, RAID_IMAGE, idx, &lvl_array[1].lv, set_error_seg))
		return_0;

	if (extracted_meta_lvs)
		dm_list_add(extracted_meta_lvs, &lvl_array[0].list);

	if (extracted_data_lvs)
		dm_list_add(extracted_data_lvs, &lvl_array[1].list);

	return 1;
}

/*
 * Remove sublvs of @type from @seg starting at @idx excluding @end and
 * put them on @removal_lvs setting mappings to "erorr" if @error_seg
 */
static int _extract_image_component_sublist(struct lv_segment *seg,
					    uint64_t type, uint32_t idx, uint32_t end,
					    struct dm_list *removal_lvs,
					    int error_seg)
{
	uint32_t s;
	struct lv_list *lvl;

	if (idx >= seg->area_count ||
	    end > seg->area_count ||
	    end <= idx) {
		log_error(INTERNAL_ERROR "area index wrong for segment");
		return 0;
	}

	if (!(lvl = dm_pool_alloc(seg_lv(seg, 0)->vg->vgmem, sizeof(*lvl) * (end - idx))))
		return_0;

	for (s = idx; s < end; s++) {
		if (!_extract_image_component_error_seg(seg, type, s, &lvl->lv, error_seg))
			return 0;

		dm_list_add(removal_lvs, &lvl->list);
		lvl++;
	}

	if (!idx && end == seg->area_count) {
		if (type == RAID_IMAGE)
			seg->areas = NULL;
		else
			seg->meta_areas = NULL;
	}

	return 1;
}

/* Extract sublvs of @type from @seg starting with @idx and put them on @removal_Lvs */
static int _extract_image_component_list(struct lv_segment *seg,
					 uint64_t type, uint32_t idx,
					 struct dm_list *removal_lvs)
{
	return _extract_image_component_sublist(seg, type, idx, seg->area_count, removal_lvs, 1);
}

/* Add new @lvs to @lv at @area_offset */
static int _add_image_component_list(struct lv_segment *seg, int delete_from_list,
				     uint64_t lv_flags, struct dm_list *lvs, uint32_t area_offset)
{
	uint32_t s = area_offset;
	struct lv_list *lvl, *tmp;

	dm_list_iterate_items_safe(lvl, tmp, lvs) {
		if (delete_from_list)
			dm_list_del(&lvl->list);

		if (lv_flags & VISIBLE_LV)
			lv_set_visible(lvl->lv);
		else
			lv_set_hidden(lvl->lv);

		if (lv_flags & LV_REBUILD)
			lvl->lv->status |= LV_REBUILD;
		else
			lvl->lv->status &= ~LV_REBUILD;

		if (!set_lv_segment_area_lv(seg, s++, lvl->lv, 0 /* le */,
					lvl->lv->status)) {
			log_error("Failed to add sublv %s", lvl->lv->name);
			return 0;
		}
	}

	return 1;
}

/*
 * Create an LV of specified type.  Set visible after creation.
 * This function does not make metadata changes.
 */
static struct logical_volume *_alloc_image_component(struct logical_volume *lv, const char *alt_base_name,
						     struct alloc_handle *ah, uint32_t first_area,
						     uint64_t type)
{
	uint64_t status = RAID | LVM_READ | LVM_WRITE | type;
	char img_name[NAME_LEN];
	const char *type_suffix;
	struct logical_volume *tmp_lv;
	const struct segment_type *segtype;

	switch (type) {
		case RAID_META:
			type_suffix = "rmeta";
			break;
		case RAID_IMAGE:
			type_suffix = "rimage";
			status |= LV_REBUILD;
			break;
		default:
			log_error(INTERNAL_ERROR "Bad type provided to %s.", __func__);
			return 0;
	}

	if (dm_snprintf(img_name, sizeof(img_name), "%s_%s_%%d",
				alt_base_name ?: lv->name, type_suffix) < 0)
		return_0;


	if (!(tmp_lv = lv_create_empty(img_name, NULL, status, ALLOC_INHERIT, lv->vg))) {
		log_error("Failed to allocate new raid component, %s.", img_name);
		return 0;
	}

	/* If no allocation requested, leave it to the empty LV (needed for striped -> raid0 takeover) */
	if (ah) {
		if (!(segtype = get_segtype_from_string(lv->vg->cmd, SEG_TYPE_NAME_STRIPED)))
			return_0;

		if (!lv_add_segment(ah, first_area, 1 /* areas */, 1 /* data_copies */,
				    tmp_lv, segtype, 0, status, 0)) {
			log_error("Failed to add segment to LV, %s", img_name);
			return 0;
		}
	}

	lv_set_visible(tmp_lv);

	return tmp_lv;
}

/* Calculate absolute amount of metadata device extens based on @rimage_extents, @region_size and @extens_size */
static uint32_t _raid_rmeta_extents(struct cmd_context *cmd, uint32_t rimage_extents,
				    uint32_t region_size, uint32_t extent_size)
{
	uint64_t bytes, regions, sectors;

	region_size = region_size ?: get_default_region_size(cmd);
	regions = (uint64_t) rimage_extents * extent_size / region_size;

	/* raid and bitmap superblocks + region bytes */
	bytes = 2 * 4096 + dm_div_up(regions, 8);
	sectors = dm_div_up(bytes, 512);

	return dm_div_up(sectors, extent_size);
}

/*
 * Returns raid metadata device size _change_ in extents, algorithm from dm-raid ("raid" target) kernel code.
 */
uint32_t raid_rmeta_extents_delta(struct cmd_context *cmd,
		uint32_t rimage_extents_cur, uint32_t rimage_extents_new,
		uint32_t region_size, uint32_t extent_size)
{
	uint32_t rmeta_extents_cur = _raid_rmeta_extents(cmd, rimage_extents_cur, region_size, extent_size);
	uint32_t rmeta_extents_new = _raid_rmeta_extents(cmd, rimage_extents_new, region_size, extent_size);
PFLA("rimage_extents_cur=%u rmeta_extents_cur=%u rimage_extents_new=%u rmeta_extents_new=%u region_size=%u extent_size=%u", rimage_extents_cur, rmeta_extents_cur,  rimage_extents_new, rmeta_extents_new, region_size, extent_size);
	/* Need minimum size on LV creation */
	if (!rimage_extents_cur)
		return rmeta_extents_new;

	/* Need current size on LV deletion */
	if (!rimage_extents_new)
		return rmeta_extents_cur;

	if (rmeta_extents_new == rmeta_extents_cur)
		return 0;

	/* Extending/reducing... */
	return rmeta_extents_new > rmeta_extents_cur ?
		rmeta_extents_new - rmeta_extents_cur :
		rmeta_extents_cur - rmeta_extents_new;
}

/*
 * _alloc_rmeta_for_lv
 * @lv
 *
 * Allocate  RAID metadata device for the given LV (which is or will
 * be the associated RAID data device).  The new metadata device must
 * be allocated from the same PV(s) as the data device.
 */
static int _alloc_rmeta_for_lv(struct logical_volume *data_lv,
			       struct logical_volume **meta_lv)
{
	int r = 1;
	char *p;
	struct dm_list allocatable_pvs;
	struct alloc_handle *ah;
	struct lv_segment *seg = first_seg(data_lv);

	dm_list_init(&allocatable_pvs);

#if 0
	/* HM FIXME: TESTME: with _dupe() */
	if (!seg_is_linear(seg)) {
		log_error(INTERNAL_ERROR "Unable to allocate RAID metadata "
				"area for non-linear LV, %s", data_lv->name);
		return 0;
	}
#endif
	_check_and_init_region_size(data_lv);

	if ((p = strstr(data_lv->name, "_mimage_")) ||
	    (p = strstr(data_lv->name, "_rimage_")))
		*p = '\0';

	if (!get_pv_list_for_lv(data_lv->vg->cmd->mem,
				data_lv, &allocatable_pvs)) {
		log_error("Failed to build list of PVs for %s", display_lvname(data_lv));
		return 0;
	}

	if (!(ah = allocate_extents(data_lv->vg, NULL, seg->segtype,
					0, 1, 0,
					seg->region_size,
					_raid_rmeta_extents(data_lv->vg->cmd, data_lv->le_count,
						seg->region_size, data_lv->vg->extent_size),
					&allocatable_pvs, data_lv->alloc, 0, NULL)))
		return_0;

	if (!(*meta_lv = _alloc_image_component(data_lv, data_lv->name, ah, 0, RAID_META)))
		r = 0;

	if (p)
		*p = '_';

	alloc_destroy(ah);

	return r;
}

/*
 * HM
 *
 * Allocate metadata devs for all @new_data_devs and link them to list @new_meta_lvs
 */
static int _alloc_rmeta_devs_for_rimage_devs(struct logical_volume *lv,
		struct dm_list *new_data_lvs,
		struct dm_list *new_meta_lvs)
{
	uint32_t a = 0, raid_devs = 0;
	struct dm_list *l;
	struct lv_list *lvl, *lvl_array;

	dm_list_iterate(l, new_data_lvs)
		raid_devs++;

PFLA("raid_devs=%u", raid_devs);

	if (!raid_devs)
		return 0;

	if (!(lvl_array = dm_pool_zalloc(lv->vg->vgmem, raid_devs * sizeof(*lvl_array))))
		return 0;

	dm_list_iterate_items(lvl, new_data_lvs) {
		log_debug_metadata("Allocating new metadata LV for %s",
				lvl->lv->name);
		if (!_alloc_rmeta_for_lv(lvl->lv, &lvl_array[a].lv)) {
			log_error("Failed to allocate metadata LV for %s in %s",
					lvl->lv->name, lv->vg->name);
			return 0;
		}

		dm_list_add(new_meta_lvs, &lvl_array[a].list);
		a++;
	}

	return 1;
}

/*
 * HM
 *
 * Allocate metadata devs for all data devs of an LV
 */
static int _alloc_rmeta_devs_for_lv(struct logical_volume *lv, struct dm_list *meta_lvs)
{
	uint32_t s;
	struct lv_list *lvl_array;
	struct dm_list data_lvs;
	struct lv_segment *seg = first_seg(lv);

	dm_list_init(&data_lvs);

	if (seg->meta_areas) {
		log_error(INTERNAL_ERROR "Metadata LVs exist in %s", display_lvname(lv));
		return 0;
	}

	if (!(seg->meta_areas = dm_pool_zalloc(lv->vg->vgmem,
					seg->area_count * sizeof(*seg->meta_areas))))
		return 0;

	if (!(lvl_array = dm_pool_alloc(lv->vg->vgmem, seg->area_count * sizeof(*lvl_array))))
		return_0;

	for (s = 0; s < seg->area_count; s++) {
		lvl_array[s].lv = seg_lv(seg, s);
		dm_list_add(&data_lvs, &lvl_array[s].list);
	}

	if (!_alloc_rmeta_devs_for_rimage_devs(lv, &data_lvs, meta_lvs)) {
		log_error("Failed to allocate metadata LVs for %s", lv->name);
		return 0;
	}

	return 1;
}

/*
 * Create @count new image component pairs for @lv and return them in
 * @new_meta_lvs and @new_data_lvs allocating space if @allocate is set.
 *
 * Use @pvs list for allocation if set, else just create empty image LVs.
 */
static int _alloc_image_components(struct logical_volume *lv,
				   struct dm_list *pvs, uint32_t count,
				   struct dm_list *meta_lvs,
				   struct dm_list *data_lvs)
{
	int r = 0;
	uint32_t s, extents, reshape_per_dev_len;
	struct lv_segment *seg = first_seg(lv);
	const struct segment_type *segtype;
	struct alloc_handle *ah;
	struct dm_list *parallel_areas;
	struct lv_list *lvl_array;

	if (!meta_lvs && !data_lvs)
		return 0;

	if (!(lvl_array = dm_pool_alloc(lv->vg->vgmem, 2 * count * sizeof(*lvl_array))))
		return_0;

	_check_and_init_region_size(lv);

	/* If this is an image addition to an existing raid set, use its type... */
	if (seg_is_raid(seg))
		segtype = seg->segtype;

	/* .. if not, set it to raid1 */
	else if (!(segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID1)))
		return_0;
PFL();

	reshape_per_dev_len = first_seg(seg_lv(seg, 0))->reshape_len;

	/*
	 * The number of extents is based on the RAID type.  For RAID1/10,
	 * each of the rimages is the same size - 'le_count'.  However
	 * for RAID 0/4/5/6, the stripes add together (NOT including the parity
	 * devices) to equal 'le_count'.  Thus, when we are allocating
	 * individual devices, we must specify how large the individual device
	 * is along with the number we want ('count').
	 */
	if (pvs) {
		uint32_t stripes, data_copies, metadata_area_count = count;

		if (!(parallel_areas = build_parallel_areas_from_lv(lv, 0, 1)))
			return_0;

		/* Amount of extents for the rimage device(s) */
		if (seg_is_any_raid10(seg)) {
			stripes = count;
			data_copies = 1;
			extents = count * lv_raid_rimage_extents(lv->le_count, seg->area_count, seg->data_copies);

		} else if (seg_is_striped_raid(seg)) {
			stripes = count;
			data_copies = 1;
			extents = count * ((lv->le_count + seg->reshape_len) / _data_rimages_count(seg, seg->area_count));

PFLA("stripes=%u lv->le_count=%u data_rimages_count=%u seg->area_count=%u", stripes, lv->le_count, _data_rimages_count(seg, seg->area_count), seg->area_count);
		} else {
			stripes = 1;
			data_copies = count;
			extents = lv->le_count;
		}

		if (!(ah = allocate_extents(lv->vg, NULL, segtype,
					    stripes, data_copies, metadata_area_count,
					    seg->region_size, extents,
					    pvs, lv->alloc, 0, parallel_areas)))
			return_0;

	} else
		ah = NULL;

PFLA("count=%u extents=%u", count, extents);

	for (s = 0; s < count; s++) {
		/*
		 * The allocation areas are grouped together.  First
		 * come the rimage allocated areas, then come the metadata
		 * allocated areas.  Thus, the metadata areas are pulled
		 * from 's + count'.
		 */

		/*
		 * If the segtype is raid0, we may avoid allocating metadata LVs
		 * to accompany the data LVs by not passing in @meta_lvs
		 */
		if (meta_lvs) {
			if (!(lvl_array[s + count].lv = _alloc_image_component(lv, NULL, ah, s + count, RAID_META)))
				goto_bad;

			dm_list_add(meta_lvs, &lvl_array[s + count].list);
		}

		if (data_lvs) {
			if (!(lvl_array[s].lv = _alloc_image_component(lv, NULL, ah, s, RAID_IMAGE)))
				goto_bad;

			dm_list_add(data_lvs, &lvl_array[s].list);
			if (lvl_array[s].lv->le_count)
				first_seg(lvl_array[s].lv)->reshape_len = reshape_per_dev_len;
		}
	}

	r =  1;
bad:
	if (ah)
		alloc_destroy(ah);

	return r;
}

/*
 * _raid_extract_images
 * @lv
 * @new_image_count:  The absolute count of images (e.g. '2' for a 2-way mirror)
 * @target_pvs:  The list of PVs that are candidates for removal
 * @shift:  If set, use _shift_image_components().
 *          Otherwise, leave the [meta_]areas as AREA_UNASSIGNED and
 *          seg->area_count unchanged.
 * @extracted_[meta|data]_lvs:  The LVs removed from the array.  If 'shift'
 *                              is set, then there will likely be name conflicts.
 * This function extracts _both_ portions of the indexed image.  It
 * does /not/ commit the results.  (IOW, erroring-out requires no unwinding
 * of operations.)
 *
 * Returns: 1 on success, 0 on failure
 */
static int _raid_extract_images(struct logical_volume *lv, uint32_t new_image_count,
				struct dm_list *target_pvs, int shift,
				struct dm_list *extracted_meta_lvs,
				struct dm_list *extracted_data_lvs)
{
	unsigned s, extract;
	struct lv_list *lvl_pairs;
	struct lv_segment *seg = first_seg(lv);
	struct segment_type *error_segtype;

	extract = seg->area_count - new_image_count;

	if ((s = dm_list_size(target_pvs)) < extract) {
		log_error("Unable to remove %d images:  Only %d device%s given.",
			  extract, s, s == 1 ? "" : "s");
		return 0;
	}

	log_verbose("Extracting %u image%s from %s", extract,
		    extract > 1 ? "s" : "", display_lvname(lv));

	if (!(lvl_pairs = dm_pool_alloc(lv->vg->vgmem, 2 * extract * sizeof(*lvl_pairs))))
		return_0;

	if (!(error_segtype = get_segtype_from_string(lv->vg->cmd, "error")))
		return_0;

	/*
	 * We make two passes over the devices.
	 * - The first pass we look for error LVs to handle them first
	 * - The second pass we look for PVs that match target_pvs and extract them
	 */
	/* First pass */
	for (s = seg->area_count; s-- && extract; ) {
		/* Conditions for first pass */
		if (first_seg(seg_metalv(seg, s))->segtype != error_segtype &&
		    first_seg(seg_lv(seg, s))->segtype     != error_segtype)
			continue;

		if (!dm_list_empty(target_pvs) && target_pvs != &lv->vg->pvs) {
			/*
			 * User has supplied a list of PVs, but we
			 * cannot honor that list because error LVs
			 * must come first.
			 */
			log_error("%s has components with error targets"
				  " that must be removed first: %s.",
				  display_lvname(lv), display_lvname(seg_lv(seg, s)));
			log_error("Try removing the PV list and rerun the command.");
			return 0;
		}

		log_debug("LVs with error segments to be removed: %s %s",
			  display_lvname(seg_metalv(seg, s)), display_lvname(seg_lv(seg, s)));

		if (!_extract_image_component_pair(seg, s, lvl_pairs, extracted_meta_lvs, extracted_data_lvs, 0))
			return_0;

		lvl_pairs += 2;
		extract--;
	}

	/* Second pass */
	for (s = seg->area_count; s-- && extract; ) {
		/* Conditions for second pass */
		if (!target_pvs)
			break;

		if (!lv_is_on_pvs(seg_metalv(seg, s), target_pvs) ||
		    !lv_is_on_pvs(seg_lv(seg, s), target_pvs))
			continue;

		if (!_raid_in_sync(lv) &&
		    (!seg_is_mirrored(seg) || !s)) {
			log_error("Unable to extract %sRAID image"
				  " while RAID array is not in-sync",
				  seg_is_mirrored(seg) ? "primary " : "");
			return 0;
		}

		if (!_extract_image_component_pair(seg, s, lvl_pairs, extracted_meta_lvs, extracted_data_lvs, 0))
			return_0;

		lvl_pairs += 2;
		extract--;
	}

	if (extract) {
		log_error("Unable to extract enough images to satisfy request");
		return 0;
	}

	if (shift && !_shift_image_components(seg)) {
		log_error("Failed to shift and rename image components");
		return 0;
	}

	return 1;
}

/*
 * Extend/reduce size of @lv and it's first segment during reshape to @extents
 */
static void _reshape_change_size(struct logical_volume *lv, uint32_t extents,
				 uint32_t old_image_count, uint32_t new_image_count)
{
	struct lv_segment *seg = first_seg(lv);
	int extend = new_image_count > old_image_count;
	int64_t s_extends = extend ? extents : -extents;

PFL();
	lv->le_count += s_extends;
	lv->size = lv->le_count * lv->vg->extent_size;
	seg->len += s_extends;
	seg->area_len += s_extends;
	seg->reshape_len = first_seg(seg_lv(seg, 0))->reshape_len * _data_rimages_count(seg, new_image_count);

	/* Extend */
	if (extend && old_image_count == 2 && !seg->stripe_size)
		seg->stripe_size = DEFAULT_STRIPESIZE;

	else if (new_image_count == 2)
		seg->stripe_size = 0;
}

/*
 * Change the image count of the raid @lv to @new_image_count
 * allocating from list @allocate_pvs and putting any removed
 * LVs on the @removal_lvs list
 */
static int _lv_change_image_count(struct logical_volume *lv, const struct segment_type *new_segtype,
				  uint32_t new_image_count,
				  struct dm_list *allocate_pvs, struct dm_list *removal_lvs)
{
	struct lv_segment *seg = first_seg(lv);
	struct dm_list meta_lvs, data_lvs;
	int reshape_disks = (seg_is_raid10_near(seg) || seg_is_raid10_offset(seg) || seg_is_striped_raid(seg)) &&
			    !seg_is_any_raid0(seg) &&
			    is_same_level(seg->segtype, new_segtype);
	uint32_t old_image_count = seg->area_count, s;

PFLA("reshape_disks=%d", reshape_disks);
	if (old_image_count == new_image_count) {
		log_warn("%s already has image count of %d.",
			 display_lvname(lv), new_image_count);
		return 1;
	}

	/* Check for maximum supported raid devices */
	if (!_check_max_raid_devices(new_image_count))
		return 0;

PFLA("reshape_disks=%u", reshape_disks);
	dm_list_init(&meta_lvs);
	dm_list_init(&data_lvs);

	if (old_image_count < new_image_count) {
		/* Allocate additional meta and data LVs pair(s) */
		log_debug_metadata("Allocating additional data and metadata LV pair for %s", display_lvname(lv));
		if (!_alloc_image_components(lv, allocate_pvs, new_image_count - old_image_count,
					     &meta_lvs, &data_lvs)) {
			log_error("Failed to allocate additional data and metadata LV pair for %s", display_lvname(lv));
			return_0;
		}

		log_debug_metadata("Clearing newly allocated metadata LVs of %s", display_lvname(lv));
		if (!_clear_lvs(&meta_lvs)) {
			log_error("Failed to clear newly allocated metadata LVs of %s", display_lvname(lv));
			return_0;
		}

		/* Grow areas arrays for metadata and data devs for adding ne image component pairs */
		log_debug_metadata("Realocating areas arrays of %s", display_lvname(lv));
		if (!_realloc_meta_and_data_seg_areas(lv, new_image_count)) {
			log_error("Relocation of areas arrays for %s failed", display_lvname(lv));
			return_0;
		}

		log_debug_metadata("Adding new data and metadata LVs to %s", display_lvname(lv));
		if (!_add_image_component_list(seg, 1, 0, &meta_lvs, old_image_count) ||
		    !_add_image_component_list(seg, 1, LV_REBUILD, &data_lvs, old_image_count)) {
			log_error("Failed to add new data and metadata LVs to %s", display_lvname(lv));
			return_0;
		} 

		if (reshape_disks) {
PFL();
			/* HM FIXME: enhance for raid10! */
			_reshape_change_size(lv,
					     (new_image_count - old_image_count) *
					      (lv->le_count / _data_rimages_count(seg, old_image_count)),
					     old_image_count, new_image_count);
			/*
		 	 * Reshape adding image component pairs:
		 	 *
		 	 * - reset rebuild flag on new image LVs
		 	 * - set delta disks plus flag on new image LVs
		 	 */
			log_debug_metadata("Setting delta disk flag on new data LVs of %s",
					   display_lvname(lv));
			for (s = old_image_count; s < new_image_count; s++) {
PFLA("seg_lv(seg, %u)=%s", s, seg_lv(seg, s)->name);
				seg_lv(seg, s)->status &= ~LV_REBUILD;
				seg_lv(seg, s)->status |= LV_RESHAPE_DELTA_DISKS_PLUS;
			}
		}

	} else {
		if (!removal_lvs) {
			log_error(INTERNAL_ERROR "Called without removal LVs list");
			return 0;
		}

		/*
		 * Extract all image and any metadata lvs past new_image_count 
		 *
		 * No need to reallocate data and metadata areas
		 * on reduction of image component pairs
		 */
		log_debug_metadata("Extracting data and metadata LVs from %s", display_lvname(lv));
		if (!_raid_extract_images(lv, new_image_count, allocate_pvs,
					  0 /* !shift */, removal_lvs, removal_lvs)) {
			log_error("Failed to extract data and metadata LVs from %s", display_lvname(lv));
			return 0;
		}

		/* Reshape removing image component pairs -> change sizes accordingly */
		if (reshape_disks)
			/* HM FIXME: enhance for raid10! */
			_reshape_change_size(lv,
					     (old_image_count - new_image_count) *
					      (lv->le_count / _data_rimages_count(seg, old_image_count)),
					     old_image_count, new_image_count);
	}

	/* Must update area count after resizing it */
	seg->area_count = new_image_count;

	return 1;
}

/* Return reshape LEs per device for @seg */
static uint32_t _reshape_les_per_dev(struct lv_segment *seg)
{
	return seg->reshape_len / _data_rimages_count(seg, seg->area_count);
}

/*
 * Relocate @out_of_place_les_per_disk from @lv's data images  begin <-> end depending on @to_end
 *
 * to_end != 0 -> begin -> end
 * to_end == 0 -> end -> begin
 */
static int _relocate_reshape_space(struct logical_volume *lv, int to_end)
{
	uint32_t le, end, s, len_per_dlv, reshape_len_per_dev;
	struct logical_volume *dlv;
	struct lv_segment *seg = first_seg(lv);
	struct lv_segment *data_seg;
	struct dm_list *where;

	if (!seg->reshape_len ||
	    !(len_per_dlv = _reshape_les_per_dev(seg))) {
		log_error(INTERNAL_ERROR "No reshape space to relocate");
		return 0;
	}

	reshape_len_per_dev = first_seg(seg_lv(seg,0))->reshape_len;

	/*
	 * Move the reshape LEs of each stripe (i.e. the data image sub lv)
	 * in the first/last segments across to new segments or just use
	 * them in case size fits
	 */
	for (s = 0; s < seg->area_count; s++) {
		dlv = seg_lv(seg, s);

		/* Move to the end -> start from 0 and end with reshape LEs */
		if (to_end) {
			le = 0;
			end = len_per_dlv;

			/* Move to the beginning -> from "end - reshape LEs" to end  */
		} else {
			le = dlv->le_count - len_per_dlv;
			end = dlv->le_count;
		}


		/* Ensure segment boundary at begin/end of reshape space */
		if (!lv_split_segment(dlv, to_end ? end : le))
			return_0;

		/* Find start segment */
		data_seg = find_seg_by_le(dlv, le);
		while (le < end) {
			struct lv_segment *n = dm_list_item(data_seg->list.n, struct lv_segment);

			le += data_seg->len;
			/* select destination to move to (begin/end) */
			where = to_end ? &dlv->segments : dlv->segments.n;
			dm_list_move(where, &data_seg->list);
			data_seg = n;
		}

		/* Adjust starting LEs of data lv segments after move */;
		le = 0;
		dm_list_iterate_items(data_seg, &dlv->segments) {
			data_seg->reshape_len = le ? 0 : reshape_len_per_dev;
			data_seg->le = le;
			le += data_seg->len;
		}
	}

	return 1;
}

/*
 * Check if we've got out of space reshape
 * capacity in @lv and allocate if necessary.
 *
 * We inquire the targets status interface to retrieve
 * the current data_offset and the device size and
 * compare that to the size of the component image LV
 * to tell if an extension of the LV is needed or
 * existing space can just be used,
 *
 * Three different scenarios need to be covered:
 *
 *  - we have to reshape forwards
 *    (true for adding disks to a raid set) ->
 *    add extent to each component image upfront
 *    or move an exisiting one at the end across;
 *    kernel will set component devs data_offset to
 *    the passed in one and new_data_offset to 0,
 *    i.e. the data starts at offset 0 after the reshape
 *
 *  - we have to reshape backwards
 *    (true for removing disks form a raid set) ->
 *    add extent to each component image by the end
 *    or use already exisiting one from a previous reshape;
 *    kernel will leave the data_offset of each component dev
 *    at 0 and set new_data_offset to the passed in one,
 *    i.e. the data will be at offset new_data_offset != 0
 *    after the reshape
 *
 *  - we are free to reshape either way
 *    (true for layout changes keeping number of disks) ->
 *    let the kernel identify free out of place reshape space
 *    and select the appropriate data_offset and 
 *    reshape direction
 *
 * Kernel will always be told to put data offset
 * on an extent boundary.
 * When we convert to mappings outside MD ones such as linear,
 * striped and mirror _and_ data_offset != 0, split the first segment
 * and adjust the rest to remove the reshape space.
 * If it's at the end, just lv_reduce() and set seg->reshape_len to 0.
 *
 * Does not write metadata!
 */
enum alloc_where { alloc_begin, alloc_end, alloc_anywhere };
static int _lv_alloc_reshape_space(struct logical_volume *lv,
				   enum alloc_where where,
				   struct dm_list *allocate_pvs)
{
	/* Reshape LEs per disk minimum one MiB for now... */
	uint32_t out_of_place_les_per_disk = max(2048ULL / (unsigned long long) lv->vg->extent_size, 1ULL);
	uint64_t data_offset, dev_sectors;
	struct lv_segment *seg = first_seg(lv);

	/* Get data_offset and dev_sectors from the kernel */
	if (!lv_raid_offset_and_sectors(lv, &data_offset, &dev_sectors)) {
		log_error("Can't get data offset and dev size for %s from kernel",
			  display_lvname(lv));
		return 0;
	}

PFLA("data_offset=%llu dev_sectors=%llu seg->reshape_len=%u out_of_place_les_per_disk=%u lv->le_count=%u", (unsigned long long) data_offset, (unsigned long long) dev_sectors, seg->reshape_len, out_of_place_les_per_disk, lv->le_count);

	/*
	 * Check if we have reshape space allocated or extend the LV to have it
	 *
	 * first_seg(lv)->reshape_len (only segment of top level raid LV)
	 * is accounting for the data rimages so that unchanged
	 * lv_extend()/lv_reduce() can be used to allocate/free
	 */
	if (!seg->reshape_len) {
		uint32_t s;
		uint32_t data_rimages = _data_rimages_count(seg, seg->area_count);
		uint32_t reshape_len = out_of_place_les_per_disk * data_rimages;

PFLA("images=%u area_count=%u reshape_len=%u", data_rimages, seg->area_count, reshape_len);
		if (!lv_extend(lv, seg->segtype, data_rimages,
				seg->stripe_size, 1, seg->region_size,
				reshape_len /* # of reshape LEs to add */,
				allocate_pvs, lv->alloc, 0)) {
			log_error("Failed to allocate out-of-place reshape space for %s.",
				  display_lvname(lv));
			return 0;
		}

		/*
		 * Store the allocated reshape length for all data images
		 * of the LV in the only segment of the top-level RAID LV
		 */
		seg->reshape_len = reshape_len;
#if 1
		for (s = 0; s < seg->area_count; s++)
			first_seg(seg_lv(seg, s))->reshape_len = out_of_place_les_per_disk;
#endif
	}

	/* Preset data offset in case we fail relocating reshape space below */
	seg->data_offset = 0;

	/*
	 * Handle reshape space relocation
	 */
PFLA("data_offset=%llu", (unsigned long long) data_offset);
	switch (where) {
		case alloc_begin:
			/* Kernel says we have it at the end -> relocate it to the begin */
			if (!data_offset && !_relocate_reshape_space(lv, 0))
				return_0;

			data_offset = _reshape_les_per_dev(seg) * lv->vg->extent_size;
			break;

		case alloc_end:
			/* Kernel says we have it at the beginning -> relocate it to the end */
			if (data_offset && !_relocate_reshape_space(lv, 1))
				return_0;

			data_offset = 0;
			break;

		case alloc_anywhere:
			/* We don't care were the space is */
			if (data_offset)
				data_offset = 0;
			else
				data_offset = _reshape_les_per_dev(seg) * lv->vg->extent_size;

			break;

		default:
			log_error(INTERNAL_ERROR "Bogus reshape space allocation request");
			return 0;
	}

	/* Inform kernel about the reshape length in sectors */
	seg->data_offset = out_of_place_les_per_disk * lv->vg->extent_size;
PFLA("data_offset=%llu", (unsigned long long) data_offset);

	/* At least try merging segments */
	return lv_merge_segments(lv);
}

/* Remove any reshape space from the data lvs of @lv */
static int _lv_free_reshape_space(struct logical_volume *lv)
{
	struct lv_segment *seg = first_seg(lv);
PFL();
	if (seg->reshape_len) {
		uint32_t s;

		/*
		 * Got reshape space on request to free it.
		 * If it happens to be at the beginning of
		 * the data LVs, remap it to the end in order
		 * to be able to free it via lv_reduce().
		 */
		if (!_lv_alloc_reshape_space(lv, alloc_end, NULL))
			return_0;
#if 1
		for (s = 0; s < seg->area_count; s++)
			first_seg(seg_lv(seg, s))->reshape_len = 0;
#endif
		if (!lv_reduce(lv, seg->reshape_len))
			return_0;

		seg->reshape_len = 0;
		seg->data_offset = 0;
	}

	return 1;
}

/*
 * Convert @lv to raid1 by making the linear lv
 * the one data sub lv of a new top-level lv
 */
static struct lv_segment *_convert_lv_to_raid1(struct logical_volume *lv, const char *suffix)
{
	struct lv_segment *seg = first_seg(lv);
	uint64_t flags = RAID | LVM_READ | (lv->status & LVM_WRITE);

	log_debug_metadata("Inserting layer lv on top of %s", display_lvname(lv));
	if (!insert_layer_for_lv(lv->vg->cmd, lv, flags, suffix))
		return NULL;

	/* First segment has changed because of layer insertion */
	seg = first_seg(lv);
	seg->status |= SEG_RAID;
	seg_lv(seg, 0)->status |= RAID_IMAGE | flags;
	seg_lv(seg, 0)->status &= ~LV_REBUILD;

	/* Set raid1 segtype, so that the following image allocation works */
	if (!(seg->segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID1)))
		return NULL;

	lv->status |= RAID;
	_check_and_init_region_size(lv);

	return seg;
}

/* Reset any rebuild or reshape disk flags on @lv, first segment already passed to the kernel */
static int _reset_flags_passed_to_kernel(struct logical_volume *lv, int *flag_cleared)
{
	uint32_t s;
	struct lv_segment *seg = first_seg(lv);
	uint64_t reset_flags = LV_REBUILD | LV_RESHAPE_DELTA_DISKS_PLUS | LV_RESHAPE_DELTA_DISKS_MINUS;

	*flag_cleared = 0;
	for (s = 0; s < seg->area_count; s++)
		if (seg_lv(seg, s)->status & reset_flags) {
			seg_lv(seg, s)->status &= ~reset_flags;
			*flag_cleared = 1;
		}

	if (seg->data_offset) {
		seg->data_offset = 0;
		*flag_cleared = 1;
	}

	if (*flag_cleared) {
		if (!vg_write(lv->vg) || !vg_commit(lv->vg)) {
			log_error("Failed to clear flags for %s components",
					display_lvname(lv));
			return 0;
		}

		backup(lv->vg);
	}

	return 1;
}

/* Area reorder helper: swap 2 LV segment areas @a1 and @a2 */
static void _swap_areas(struct lv_segment_area *a1, struct lv_segment_area *a2)
{
	struct lv_segment_area tmp;
	char *tmp_name;

	tmp = *a1;
	*a1 = *a2;
	*a2 = tmp;
#if 0
	/* Rename LVs ? */
	tmp_name = a1->u.lv.lv->name;
	a1->u.lv.lv->name = a2->u.lv.lv->name;
	a2->u.lv.lv->name = tmp_name;
#endif
}

/*
 * HM FIXME: leverage to support more raid10 mirror images
 *
 * Reorder the areas in the first segment of @seg to suit raid10/raid0 layout
 *
 * Examples with 6 disks indexed 0..5 with 3 stripes:
 * raid0  (012345) -> raid10 (031425) order
 * idx     024135
 * raid10 (012345) -> raid0  (024135/135024) order depending on mirror leg selection (TBD)
 * idx     031425
 * _or_ (variations possible)
 * idx     304152
 *
 * Examples 3 stripes with 9 disks indexed 0..8 to create a 3 striped raid0 with 3 data_copies per leg:
 *         vvv
 * raid0  (012345678) -> raid10 (034156278) order
 *         v  v  v
 * raid10 (012345678) -> raid0  (036124578) order depending on mirror leg selection (TBD)
 *
 */
enum raid0_raid10_conversion { reorder_to_raid10, reorder_from_raid10 };
static int _raid10_reorder_seg_areas(struct lv_segment *seg, enum raid0_raid10_conversion conv)
{
	unsigned idx1, idx2, s, ss, xchg;
	uint32_t stripes = seg->area_count;
	unsigned short *idx;
	unsigned i = 0;

PFLA("seg->data_copies=%u", seg->data_copies);
	if (seg->data_copies < 2)
		seg->data_copies = 2;
PFLA("seg->data_copies=%u", seg->data_copies);

	if (stripes % seg->data_copies) {
		log_error(INTERNAL_ERROR "Can't reorder raid10 near with odd number of devices");
		return 0;
	}

	stripes /= seg->data_copies ?: 2;

PFLA("stripes=%u", stripes);
	if (!(idx = dm_pool_zalloc(seg_lv(seg, 0)->vg->vgmem, seg->area_count * sizeof(*idx))))
		return 0;

	/* Set up positional index array */
	switch (conv) {
	case reorder_to_raid10:
		// raid0  (012345)    -> raid10 (031425)    order
		// raid0  (012345678) -> raid10 (034156278) order
		// 0 -> 0
		// 1 -> 2
		// 2 -> 4
		// 3 -> 1
		// 4 -> 3
		// 5 -> 5
		ss = 1;
		for (s = 0; s < seg->area_count; s++)
			idx[s] = (s < stripes) ? s * seg->data_copies : (ss++ * seg->data_copies - 1);

		break;

	case reorder_from_raid10:
		// raid10 (012345) -> raid0  (024135/135024) order depending on mirror leg selection (TBD)
		// 0 -> 0
		// 1 -> 3
		// 2 -> 1
		// 3 -> 4
		// 4 -> 2
		// 5 -> 5
		idx1 = stripes;
		idx2 = 0;
		for (s = 0; s < seg->area_count; s++)
			idx[s] = (s % seg->data_copies) ? idx1++ : idx2++;
	}
PFL();
for (s = 0; s < seg->area_count ; s++)
PFLA("s=%u idx[s]=%u", s, idx[s]);

	/* Sort areas */
	do {
		xchg = seg->area_count;

		for (s = 0; s < seg->area_count ; s++)
			if (idx[s] == s)
				xchg--;

			else {
				_swap_areas(seg->areas + s, seg->areas + idx[s]);
				_swap_areas(seg->meta_areas + s, seg->meta_areas + idx[s]);
				ss = idx[idx[s]];
				idx[idx[s]] = idx[s];
				idx[s] = ss;
			}
		i++;
	} while (xchg);

for (s = 0; s < seg->area_count ; s++)
PFLA("s=%u idx[s]=%u", s, idx[s]);
PFLA("%d iterations", i);
for (s = 0; s < seg->area_count; s++)
PFLA("seg_lv(seg, %u)->name=%s", s, seg_lv(seg, s)->name);

	return 1;
}

/* Write vg of @lv, suspend @lv and commit the vg */
static int _vg_write_lv_suspend_vg_commit(struct logical_volume *lv)
{
	if (!vg_write(lv->vg)) {
		log_error("Failed to write changes to %s in %s",
			  lv->name, lv->vg->name);
		return 0;
	}

	if (!suspend_lv(lv->vg->cmd, lv)) {
		log_error("Failed to suspend %s before committing changes",
			  display_lvname(lv));
		vg_revert(lv->vg);
		return 0;
	}

	if (!vg_commit(lv->vg)) {
		log_error("Failed to commit changes to %s in %s",
			  lv->name, lv->vg->name);
		return 0;
	}

	return 1;
}

/****************************************************************************/
/*
 * HM
 *
 * Add/remove metadata areas to/from raid0
 *
 * Update metadata and reload mappings if @update_and_reload
 */
static int _alloc_and_add_rmeta_devs_for_lv(struct logical_volume *lv)
{
	struct lv_segment *seg = first_seg(lv);
	struct dm_list meta_lvs;

	dm_list_init(&meta_lvs);

	log_debug_metadata("Allocating metadata LVs for %s", display_lvname(lv));
	if (!_alloc_rmeta_devs_for_lv(lv, &meta_lvs)) {
		log_error("Failed to allocate metadata LVs for %s", display_lvname(lv));
		return_0;
	}

	/* Metadata LVs must be cleared before being added to the array */
	log_debug_metadata("Clearing newly allocated metadata LVs for %s", display_lvname(lv));
	if (!_clear_lvs(&meta_lvs)) {
		log_error("Failed to initialize metadata LVs for %s", display_lvname(lv));
		return_0;
	}

	/* Set segment areas for metadata sub_lvs */
	log_debug_metadata("Adding newly allocated metadata LVs to %s", display_lvname(lv));
	if (!_add_image_component_list(seg, 1, 0, &meta_lvs, 0)) {
		log_error("Failed to add newly allocated metadata LVs to %s", display_lvname(lv));
		return_0;
	}

	return 1;
}

/*
 * HM
 *
 * Add/remove metadata areas to/from raid0
 *
 * Update metadata and reload mappings if @update_and_reload
 */
static int _raid0_add_or_remove_metadata_lvs(struct logical_volume *lv,
					     int update_and_reload,
					     struct dm_list *removal_lvs)
{
	uint64_t raid_type_flag;
	struct lv_segment *seg = first_seg(lv);

	if (seg->meta_areas) {
PFL();
		log_debug_metadata("Extracting metadata LVs");
		if (!removal_lvs) {
			log_error(INTERNAL_ERROR "Called with NULL removal LVs list");
			return 0;
		}
PFL();

		if (!_extract_image_component_list(seg, RAID_META, 0, removal_lvs)) {
			log_error(INTERNAL_ERROR "Failed to extract metadata LVs");
			return 0;
		}
PFL();
		raid_type_flag = SEG_RAID0;

	} else {
		if (!_alloc_and_add_rmeta_devs_for_lv(lv))
			return 0;

		raid_type_flag = SEG_RAID0_META;
	}

	if (!(seg->segtype = get_segtype_from_flag(lv->vg->cmd, raid_type_flag)))
		return_0;
PFL();

	if (update_and_reload) {
		if (!lv_update_and_reload_origin(lv))
			return_0;

		/* If any residual LVs, eliminate them, write VG, commit it and take a backup */
		return _eliminate_extracted_lvs(lv->vg, removal_lvs);
	}

PFL();
	return 1;
}

/* Set segment area data image LVs from @data_lvs with @status in @lv and give them proper names */
static int _set_lv_areas_from_data_lvs_and_create_names(struct logical_volume *lv,
							struct dm_list *data_lvs,
							uint64_t status)
{
	uint32_t s = 0;
	char **name;
	const char *suffix = (status & RAID_IMAGE) ? "rimage" : "rmeta";
	struct lv_list *lvl, *tlvl;
	struct lv_segment *seg = first_seg(lv);

	dm_list_iterate_items_safe(lvl, tlvl, data_lvs) {
PFLA("lv=%s", display_lvname(lvl->lv));
		dm_list_del(&lvl->list);
		lv_set_hidden(lvl->lv);

		if (!set_lv_segment_area_lv(seg, s, lvl->lv, 0, status | RAID_IMAGE))
			return_0;

		name = (status & RAID_IMAGE) ? (char **) &seg_lv(seg, s)->name :
					       (char **) &seg_metalv(seg, s)->name;
		*name = _generate_raid_name(lv, suffix, s++);
		if (!*name) {
			log_error("Failed to allocate new data image lv name for %s", display_lvname(lv));
			return 0;
		}
	}

	return 1;
}

/*
 * Split off raid1 images of @lv, prefix with @split_name,
 * leave @new_image_count in the raid1 set and find them on @splittable_pvs
 */
static int _lv_update_and_reload_origin_eliminate_lvs(struct logical_volume *lv,
						      struct dm_list *removal_lvs);
int lv_raid_split(struct logical_volume *lv, const char *split_name,
		  uint32_t new_image_count, struct dm_list *splittable_pvs)
{
	uint32_t split_count;
	struct lv_list *lvl;
	struct dm_list meta_lvs, data_lvs;
	struct cmd_context *cmd = lv->vg->cmd;
	struct logical_volume *tracking, *split_lv = NULL;
	struct lv_segment *seg = first_seg(lv);
	struct dm_list tracking_pvs;

	dm_list_init(&meta_lvs);
	dm_list_init(&data_lvs);

	if (!new_image_count) {
		log_error("Unable to split all images from %s",
				display_lvname(lv));
		return 0;
	}

	if (!seg_is_raid1(seg)) {
		log_error("Unable to split logical volume of segment type, %s",
			  lvseg_name(seg));
		return 0;
	}

	if (vg_is_clustered(lv->vg) && !lv_is_active_exclusive_locally(lv)) {
		log_error("%s must be active exclusive locally to"
				" perform this operation.", display_lvname(lv));
		return 0;
	}

	if (find_lv_in_vg(lv->vg, split_name)) {
		log_error("Logical Volume \"%s\" already exists in %s",
				split_name, lv->vg->name);
		return 0;
	}

	if (!_raid_in_sync(lv)) {
		log_error("Unable to split %s while it is not in-sync.",
				display_lvname(lv));
		return 0;
	}

	/*
	 * We only allow a split while there is tracking if it is to
	 * complete the split of the tracking sub-LV
	 */
	if (_lv_is_raid_with_tracking(lv, &tracking)) {
		if (!lv_is_on_pvs(tracking, splittable_pvs)) {
			log_error("Unable to split additional image from %s "
					"while tracking changes for %s",
					lv->name, tracking->name);
			return 0;
		}

		/* Ensure we only split the tracking image */
		dm_list_init(&tracking_pvs);
		splittable_pvs = &tracking_pvs;
		if (!get_pv_list_for_lv(tracking->vg->cmd->mem,
					tracking, splittable_pvs))
			return_0;
	}

	split_count = seg->area_count - new_image_count;

	if (!_raid_extract_images(lv, new_image_count, splittable_pvs, 0,
				  &meta_lvs, &data_lvs)) {
		log_error("Failed to extract images from %s",
				display_lvname(lv));
		return 0;
	}

	/* Convert to linear? */
	if (new_image_count == 1 &&
	    !_convert_raid_to_linear(lv, &meta_lvs)) {
		log_error("Failed to remove RAID layer after linear conversion");
		return 0;
	}

	/* Rename all extracted rimages with @split_name prefix */
	dm_list_iterate_items(lvl, &data_lvs)
		if (!_lv_name_add_string_index(cmd, &lvl->lv->name, split_name))
			return 0;

	/* Split off multiple images as a seperate raid1 LV */
	if (split_count > 1) {
		uint64_t status = RAID | LVM_READ | LVM_WRITE;
		struct lv_segment *raid1_seg;

		log_warn("Splitting off %u images into new raid1 LV %s/%s",
			 split_count, lv->vg->name, split_name);

		/* Create empty LV with @split_name to add segment and images */
		if (!(split_lv = lv_create_empty(split_name, NULL, status | VISIBLE_LV, ALLOC_INHERIT, lv->vg))) {
			log_error("Failed to create new raid1 LV %s/%s.", lv->vg->name, split_name);
			return_0;
		}

		/* Create the one top-level segment for our raid1 split LV and add it to the LV  */
		if (!(raid1_seg = alloc_lv_segment(seg->segtype, split_lv, 0, seg->len, 0, status,
						   seg->stripe_size, NULL,
						   split_count, seg->area_len,
						   split_count, 0, seg->region_size, 0, NULL))) {
			log_error("Failed to create raid1 segment for %s", display_lvname(split_lv));
			return_0;
		}
		dm_list_add(&split_lv->segments, &raid1_seg->list);

		/* Set segment area data image LVs and give them proper names */
		if(!_set_lv_areas_from_data_lvs_and_create_names(split_lv, &data_lvs, status | RAID_IMAGE) ||
		   !_set_lv_areas_from_data_lvs_and_create_names(split_lv, &meta_lvs, status | RAID_META))
			return 0;

		split_lv->le_count = seg->len;
		split_lv->size = seg->len * lv->vg->extent_size;
PFLA("split_lv->le_count=%u", split_lv->le_count);
	} 

	seg->area_count = new_image_count;
	seg->data_copies = new_image_count;

	if (!_vg_write_lv_suspend_vg_commit(lv))
		return 0;

	dm_list_iterate_items(lvl, &data_lvs)
		if (!activate_lv_excl_local(cmd, lvl->lv))

	dm_list_iterate_items(lvl, &meta_lvs)
		if (!activate_lv_excl_local(cmd, lvl->lv))
			return_0;

	if (!resume_lv(lv->vg->cmd, lv_lock_holder(lv))) {
		log_error("Failed to resume %s after committing changes",
			  display_lvname(lv));
		return 0;
	}

	if (!_eliminate_extracted_lvs(lv->vg, &meta_lvs))
		return 0;

	if (split_lv && !activate_lv_excl_local(cmd, split_lv))
		return 0;

	return 1;
}


/*
 * lv_raid_split_and_track
 * @lv
 * @splittable_pvs
 *
 * Only allows a single image to be split while tracking.  The image
 * never actually leaves the mirror.  It is simply made visible.  This
 * action triggers two things: 1) users are able to access the (data) image
 * and 2) lower layers replace images marked with a visible flag with
 * error targets.
 *
 * Returns: 1 on success, 0 on error
 */
int lv_raid_split_and_track(struct logical_volume *lv,
			    struct dm_list *splittable_pvs)
{
	int s;
	struct lv_segment *seg = first_seg(lv);

	if (!seg_is_mirrored(seg)) {
		log_error("Unable to split images from non-mirrored RAID");
		return 0;
	}

	if (!_raid_in_sync(lv)) {
		log_error("Unable to split image from %s while not in-sync",
			  display_lvname(lv));
		return 0;
	}

	/* Cannot track two split images at once */
	if (lv_is_raid_with_tracking(lv)) {
		log_error("Cannot track more than one split image at a time");
		return 0;
	}

	if (seg->area_count < 3) {
		log_error("Tracking an image in 2-way raid1 LV %s would cause loosing redundancy!",
			  display_lvname(lv));
		log_error("Run \"lvconvert -m2 %s\" to have 3 legs and redo",
			  display_lvname(lv));
		return 0;
	}

	for (s = seg->area_count - 1; s >= 0; --s)
		if (lv_is_on_pvs(seg_lv(seg, s), splittable_pvs)) {
			lv_set_visible(seg_lv(seg, s));
			seg_lv(seg, s)->status &= ~LVM_WRITE;
			break;
		}

	if (s < 0) {
		log_error("Unable to find image to satisfy request");
		return 0;
	}

	if (!lv_update_and_reload(lv))
		return_0;

	log_print_unless_silent("%s split from %s for read-only purposes.",
				seg_lv(seg, s)->name, lv->name);

	/* Activate the split (and tracking) LV */
	if (!_activate_sublv_preserving_excl(lv, seg_lv(seg, s)))
		return_0;

	log_print_unless_silent("Use 'lvconvert --merge %s' to merge back into %s",
				display_lvname(seg_lv(seg, s)), lv->name);
	return 1;
}

/*
 * Merge split of tracking @image_lv back into raid1 set
 */
int lv_raid_merge(struct logical_volume *image_lv)
{
	uint32_t s;
	char *p, *lv_name;
	struct lv_list *lvl;
	struct logical_volume *lv;
	struct logical_volume *meta_lv = NULL;
	struct lv_segment *seg;
	struct volume_group *vg = image_lv->vg;
	struct logical_volume *tracking;

	if (!(lv_name = dm_pool_strdup(vg->vgmem, image_lv->name)))
		return_0;

	if (!(p = strstr(lv_name, "_rimage_"))) {
		log_error("Unable to merge non-mirror image %s.",
			  display_lvname(image_lv));
		return 0;
	}
	*p = '\0'; /* lv_name is now that of top-level RAID */

	if (!(lvl = find_lv_in_vg(vg, lv_name))) {
		log_error("Unable to find containing RAID array for %s.",
			  display_lvname(image_lv));
		return 0;
	}

	lv = lvl->lv;
	seg = first_seg(lv);

	if (!seg_is_raid1(seg)) {
		log_error("%s is no RAID1 array - refusing to merge.",
			  display_lvname(lv));
		return 0;
	}

	if (!_lv_is_raid_with_tracking(lv, &tracking)) {
		log_error("%s is not a tracking LV.",
			  display_lvname(lv));
		return 0;
	}

	if (tracking != image_lv) {
		log_error("%s is not the tracking LV of %s but %s is.",
			  display_lvname(image_lv), display_lvname(lv), display_lvname(tracking));
		return 0;
	}

	/* Image LVs should not be resizable directly, but... */
	if (seg->len != image_lv->le_count)  {
		log_error(INTERNAL_ERROR "The image LV %s of %s has different size!",
			  display_lvname(image_lv), display_lvname(lv));
		return 0;
	}

	if (image_lv->status & LVM_WRITE) {
		log_error("%s is not read-only - refusing to merge.",
			  display_lvname(image_lv));
	}

	for (s = 0; s < seg->area_count; ++s)
		if (seg_lv(seg, s) == image_lv) {
			meta_lv = seg_metalv(seg, s);
			break;
		}

	if (!meta_lv) {
		log_error("Failed to find meta for %s in RAID array %s.",
			  display_lvname(image_lv),
			  display_lvname(lv));
		return 0;
	}

	if (!deactivate_lv(vg->cmd, meta_lv)) {
		log_error("Failed to deactivate %s before merging.",
			  display_lvname(meta_lv));
		return 0;
	}

	if (!deactivate_lv(vg->cmd, image_lv)) {
		log_error("Failed to deactivate %s before merging.",
			  display_lvname(image_lv));
		return 0;
	}

	image_lv->status |= (lv->status & LVM_WRITE);
	image_lv->status |= RAID_IMAGE;
	lv_set_hidden(image_lv);
	seg->data_copies++;

	if (!lv_update_and_reload(lv))
		return_0;

	log_print_unless_silent("%s successfully merged back into %s",
				display_lvname(image_lv), display_lvname(lv));
	return 1;
}

/*
 * Adjust all data sub LVs of @lv to mirror
 * or raid name depending on @direction
 * adjusting their lv status
 */
enum mirror_raid_conv { mirror_to_raid1 = 0, raid1_to_mirror };
static int _adjust_data_lvs(struct logical_volume *lv, enum mirror_raid_conv direction)
{
	uint32_t s;
	char *p;
	struct lv_segment *seg = first_seg(lv);
	static struct {
		char type_char;
		uint64_t set_flag;
		uint64_t reset_flag;
	} conv[] = {
		{ 'r', RAID_IMAGE  , MIRROR_IMAGE },
		{ 'm', MIRROR_IMAGE, RAID_IMAGE }
	};

	for (s = 0; s < seg->area_count; ++s) {
		struct logical_volume *dlv = seg_lv(seg, s);

		if (!((p = strstr(dlv->name, "_mimage_")) ||
		      (p = strstr(dlv->name, "_rimage_")))) {
			log_error(INTERNAL_ERROR "name lags image part");
			return 0;
		}

		*(p+1) = conv[direction].type_char;
		log_debug_metadata("data lv renamed to %s", dlv->name);

		dlv->status &= ~conv[direction].reset_flag;
		dlv->status |= conv[direction].set_flag;
	}

	return 1;
}

/*
 * Convert @lv with "mirror" mapping to "raid1"
 * opitonally changing number of data_copies
 * defined by @new_image_count.
 *
 * Returns: 1 on success, 0 on failure
 */
/* HM FIXME: support leg count change */
static int _convert_mirror_to_raid(struct logical_volume *lv,
				   const struct segment_type *new_segtype,
				   uint32_t new_image_count,
				   struct dm_list *allocate_pvs,
				   int update_and_reload,
				   struct dm_list *removal_lvs)
{
	struct lv_segment *seg = first_seg(lv);

	if (!seg_is_mirrored(seg)) {
		log_error(INTERNAL_ERROR "mirror conversion supported only");
		return 0;
	}

	new_image_count = new_image_count ?: seg->area_count;
	if (new_image_count < 2) {
		log_error("can't reduce to lees than 2 data_copies");
		return 0;
	}

	/* Remove any mirror log */
	if (seg->log_lv) {
		log_debug_metadata("Removing mirror log, %s", seg->log_lv->name);
		if (!remove_mirror_log(lv->vg->cmd, lv, NULL, 0)) {
			log_error("Failed to remove mirror log");
			return 0;
		}
	}

	/* Allocate metadata devs for all mimage ones (writes+commits metadata) */
	if (!_alloc_and_add_rmeta_devs_for_lv(lv))
		return 0;

	/* Rename all data sub lvs from "*_mimage_*" to "*_rimage_*" and set their status */
	log_debug_metadata("Adjust data LVs of %s", display_lvname(lv));
	if (!_adjust_data_lvs(lv, mirror_to_raid1))
		return 0;

	init_mirror_in_sync(1);

	seg->segtype = new_segtype;
	lv->status &= ~(MIRROR | MIRRORED);
	lv->status |= RAID;
	seg->status |= RAID;

	/* Change image pair count to requested # of images */
	if (new_image_count != seg->area_count) {
		log_debug_metadata("Changing image count to %u on %s",
				   new_image_count, display_lvname(lv));
		if (!_lv_change_image_count(lv, new_segtype, new_image_count, allocate_pvs, removal_lvs))
			return 0;
	}

	return update_and_reload ? _lv_update_and_reload_origin_eliminate_lvs(lv, removal_lvs) : 1;
}

/*
 * Convert @lv with "raid1" mapping to "mirror"
 * optionally changing number of data_copies
 * defined by @new_image_count.
 *
 * Returns: 1 on success, 0 on failure
 */
static int _convert_raid1_to_mirror(struct logical_volume *lv,
				    const struct segment_type *new_segtype,
				    uint32_t new_image_count,
				    struct dm_list *allocate_pvs,
				    int update_and_reload,
				    struct dm_list *removal_lvs)
{
	struct lv_segment *seg = first_seg(lv);

	if (!seg_is_raid1(seg)) {
		log_error(INTERNAL_ERROR "raid1 conversion supported only");
		return 0;
	}

	new_image_count = new_image_count ?: seg->area_count;
	if (new_image_count < 2) {
		log_error("can't reduce to lees than 2 data_copies");
		return 0;
	}

	if (!_check_max_mirror_devices(new_image_count)) {
		log_error("Unable to convert %s LV %s with %u images to %s",
			  SEG_TYPE_NAME_RAID1, display_lvname(lv), new_image_count, SEG_TYPE_NAME_MIRROR);
		log_error("Please, at least reduce to the maximum of %u images with \"lvconvert -m%u %s\"",
			  DEFAULT_MIRROR_MAX_IMAGES, DEFAULT_MIRROR_MAX_IMAGES - 1, display_lvname(lv));
		return 0;
	}

	/* Change image pair count to requested # of images */
	if (new_image_count != seg->area_count) {
		log_debug_metadata("Changing image count to %u on %s",
				   new_image_count, display_lvname(lv));
		if (!_lv_change_image_count(lv, new_segtype, new_image_count, allocate_pvs, removal_lvs))
			return 0;
	}

	/* Remove rmeta LVs */
	log_debug_metadata("Extracting and renaming metadata LVs");
	if (!_extract_image_component_list(seg, RAID_META, 0, removal_lvs))
		return 0;

	seg->meta_areas = NULL;

	/* Rename all data sub lvs from "*_rimage_*" to "*_mimage_*" and set their status */
	log_debug_metadata("Adjust data LVs of %s", display_lvname(lv));
	if (!_adjust_data_lvs(lv, raid1_to_mirror))
		return 0;

	seg->segtype = new_segtype;
	lv->status &= ~RAID;
	seg->status &= ~RAID;
	lv->status |= (MIRROR | MIRRORED);

PFL();
	/* Add mirror_log LV */
	if (!add_mirror_log(lv->vg->cmd, lv, 1, seg->region_size, allocate_pvs, lv->vg->alloc)) {
		log_error("Unable to add mirror log to %s", display_lvname(lv));
		return 0;
	}

PFL();
	return update_and_reload ? _lv_update_and_reload_origin_eliminate_lvs(lv, removal_lvs) : 1;
}

/* BEGIN: striped -> raid0 conversion */
/*
 * HM
 *
 * Helper convert striped to raid0
 *
 * For @lv, empty hidden LVs in @data_lvs have been created by the caller.
 *
 * All areas from @lv segments are being moved to new
 * segments allocated with area_count=1 for @data_lvs.
 *
 * Returns: 1 on success, 0 on failure
 */
static int _striped_to_raid0_move_segs_to_raid0_lvs(struct logical_volume *lv,
						    struct dm_list *data_lvs)
{
	uint32_t s = 0, le;
	struct logical_volume *dlv;
	struct lv_segment *seg_from, *seg_new;
	struct lv_list *lvl;
	struct segment_type *segtype;

	if (!(segtype = get_segtype_from_string(lv->vg->cmd, SEG_TYPE_NAME_STRIPED)))
		return_0;

	dm_list_iterate_items(lvl, data_lvs)  {
		dlv = lvl->lv;
		le = 0;
		dm_list_iterate_items(seg_from, &lv->segments) {
			uint64_t status = RAID | SEG_RAID | (seg_from->status & (LVM_READ | LVM_WRITE));

			/* Allocate a segment with one area for each segment in the striped LV */
			if (!(seg_new = alloc_lv_segment(segtype, dlv,
							 le, seg_from->area_len - seg_from->reshape_len,
							 seg_from->reshape_len, status,
							 seg_from->stripe_size, NULL, 1 /* area_count */,
							 seg_from->area_len, seg_from->data_copies,
							 seg_from->chunk_size, 0 /* region_size */, 0, NULL)))
				return_0;

			seg_type(seg_new, 0) = AREA_UNASSIGNED;
			dm_list_add(&dlv->segments, &seg_new->list);
			le += seg_from->area_len;

			/* Move the respective area across to our new segment */
			if (!move_lv_segment_area(seg_new, 0, seg_from, s))
				return_0;
		}

		/* Adjust le count and lv size */
		dlv->le_count = le;
		dlv->size = (uint64_t) le * lv->vg->extent_size;
		s++;
	}

	/* Remove the empty segments from the striped LV */
	dm_list_init(&lv->segments);

	return 1;
}

/*
 * HM Helper: check that @lv has one stripe one, i.e. same stripe count in all of its segments
 *
 * Superfluous if different stripe zones will ever be supported
 */
static int _lv_has_one_stripe_zone(struct logical_volume *lv)
{
	struct lv_segment *seg;
	unsigned area_count = first_seg(lv)->area_count;

	dm_list_iterate_items(seg, &lv->segments)
		if (seg->area_count != area_count)
			return 0;

	return 1;
}

/* HM Helper: check that @lv has segments with just @areas */
static int _lv_has_segments_with_n_areas(struct logical_volume *lv, unsigned areas)
{
	struct lv_segment *seg;

	dm_list_iterate_items(seg, &lv->segments)
		if (seg->area_count != areas) {
			log_error("Called on %s with segments != %u area", display_lvname(lv), areas);
			return 0;
		}

	return 1;
}


/*
 * HM
 *
 * Helper: convert striped to raid0
 *
 * Inserts hidden LVs for all segments and the parallel areas in @lv and moves 
 * given segments and areas across.
 *
 * Optionally allocates metadata devs if @alloc_metadata_devs
 * Optionally updates metadata and reloads mappings if @update_and_reload
 *
 * Returns: 1 on success, 0 on failure
 */
static struct lv_segment *_convert_striped_to_raid0(struct logical_volume *lv,
						    int alloc_metadata_devs,
						    int update_and_reload)
{
	struct lv_segment *seg = first_seg(lv), *raid0_seg;
	unsigned area_count = seg->area_count;
	struct segment_type *segtype;
	struct dm_list data_lvs;

	if (!seg_is_striped(seg)) {
		log_error(INTERNAL_ERROR "Cannot convert non-%s LV %s to %s",
			  SEG_TYPE_NAME_STRIPED, display_lvname(lv), SEG_TYPE_NAME_RAID0);
		return NULL;
	}

	/* Check for not (yet) supported varying area_count on multi-segment striped LVs */
	if (!_lv_has_one_stripe_zone(lv)) {
		log_error("Cannot convert striped LV %s with varying stripe count to raid0",
			  display_lvname(lv));
		return NULL;
	}

	if (!(segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID0)))
		return_NULL;

	/* Allocate empty rimage components */
	dm_list_init(&data_lvs);
	if (!_alloc_image_components(lv, NULL, area_count, NULL, &data_lvs)) {
		log_error("Failed to allocate empty image components for raid0 LV %s.",
			  display_lvname(lv));
		return_NULL;
	}

	/* Move the AREA_PV areas across to the new rimage components; empties lv->segments */
	if (!_striped_to_raid0_move_segs_to_raid0_lvs(lv, &data_lvs)) {
		log_error("Failed to insert linear LVs underneath %s.", display_lvname(lv));
		return_NULL;
	}

	/*
	 * Allocate single segment to hold the image component
	 * areas based on the first data LVs properties derived
	 * from the first new raid0 LVs first segment
	 */
	seg = first_seg(dm_list_item(dm_list_first(&data_lvs), struct lv_list)->lv);
	if (!(raid0_seg = alloc_lv_segment(segtype, lv,
					   0 /* le */, lv->le_count /* len */,
					   0 /* reshape_len */, seg->status,
					   seg->stripe_size, NULL /* log_lv */,
					   area_count, lv->le_count,
					   seg->data_copies, seg->chunk_size,
					   0 /* seg->region_size */, 0u /* extents_copied */ ,
					   NULL /* pvmove_source_seg */))) {
		log_error("Failed to allocate new raid0 segement for LV %s.", display_lvname(lv));
		return_NULL;
	}

	/* Add new single raid0 segment to emptied LV segments list */
	dm_list_add(&lv->segments, &raid0_seg->list);

	/* Add data lvs to the top-level lvs segment; resets LV_REBUILD flag on them */
	if (!_add_image_component_list(raid0_seg, 1, 0, &data_lvs, 0))
		return NULL;

	lv->status |= RAID;

	/* Allocate metadata lvs if requested */
	if (alloc_metadata_devs && !_raid0_add_or_remove_metadata_lvs(lv, 0, NULL))
		return NULL;

	if (update_and_reload && !lv_update_and_reload(lv))
		return NULL;

	return raid0_seg;
}
/* END: striped -> raid0 conversion */

/* BEGIN: raid0 -> striped conversion */
/* HM Helper: walk the segment lvs of a segment @seg and find smallest area at offset @area_le */
static uint32_t _smallest_segment_lvs_area(struct lv_segment *seg, uint32_t area_le)
{
	uint32_t r = ~0, s;

	/* Find smallest segment of each of the data image lvs at offset area_le */
	for (s = 0; s < seg->area_count; s++) {
		struct lv_segment *seg1 = find_seg_by_le(seg_lv(seg, s), area_le);

		r = min(r, seg1->len); //  - area_le);
	}

	return r;
}

/* HM Helper: Split segments in segment LVs in all areas of @seg at offset @area_le */
static int _split_area_lvs_segments(struct lv_segment *seg, uint32_t area_le)
{
	uint32_t s;

	/* Make sure that there's segments starting at area_le in all data LVs */
	for (s = 0; s < seg->area_count; s++)
		if (area_le < seg_lv(seg, s)->le_count)
			if (!lv_split_segment(seg_lv(seg, s), area_le)) {
				log_error(INTERNAL_ERROR "splitting data lv segment");
				return_0;
			}

	return 1;
}

/* HM Helper: allocate a new striped segment and add it to list @new_segments */
static int _alloc_and_add_new_striped_segment(struct logical_volume *lv,
					      uint32_t le, uint32_t area_len,
					      struct dm_list *new_segments)
{
	struct lv_segment *seg = first_seg(lv), *new_seg;
	struct segment_type *striped_segtype;

	if (!(striped_segtype = get_segtype_from_string(lv->vg->cmd, SEG_TYPE_NAME_STRIPED)))
		return_0;

	/* Allocate a segment with seg->area_count areas */
	if (!(new_seg = alloc_lv_segment(striped_segtype, lv, le, area_len * seg->area_count,
					 0 /* seg->reshape_len */, seg->status & ~RAID,
					 seg->stripe_size, NULL, seg->area_count,
					 area_len, seg->data_copies, seg->chunk_size, 0, 0, NULL)))
		return_0;

	dm_list_add(new_segments, &new_seg->list);

	return 1;
}

/*
 * HM
 *
 * All areas from @lv image component LV's segments are
 * being split at "striped" compatible boundaries and
 * moved to @new_segments allocated.
 *
 * The metadata+data component LVs are being mapped to an
 * error target and linked to @removal_lvs for disposal
 * by the caller.
 *
 * Returns: 1 on success, 0 on failure
 */
static int _raid0_to_striped_retrieve_segments_and_lvs(struct logical_volume *lv,
						       struct dm_list *removal_lvs)
{
	uint32_t s, area_le, area_len, le;
	struct lv_segment *data_seg, *seg = first_seg(lv), *seg_to;
	struct dm_list new_segments;

	dm_list_init(&new_segments);

	/*
	 * Walk all segments of all data LVs splitting them up at proper boundaries
	 * and create the number of new striped segments we need to move them across
	 */
	area_le = le = 0;
	while (le < lv->le_count) {
		area_len = _smallest_segment_lvs_area(seg, area_le);
		area_le += area_len;
PFLA("area_len=%u area_le=%u area_count=%u", area_len, area_le, seg->area_count);

		if (!_split_area_lvs_segments(seg, area_le) ||
		    !_alloc_and_add_new_striped_segment(lv, le, area_len, &new_segments))
			return 0;

		le += area_len * seg->area_count;
	}

	/* Now move the prepared split areas across to the new segments */
	area_le = 0;
	dm_list_iterate_items(seg_to, &new_segments) {
		for (s = 0; s < seg->area_count; s++) {
			data_seg = find_seg_by_le(seg_lv(seg, s), area_le);

			/* Move the respective area across to our new segments area */
			if (!move_lv_segment_area(seg_to, s, data_seg, 0))
				return_0;
		}

		/* Presumes all data LVs have equal size */
		area_le += data_seg->len;
	}

	/* Extract any metadata LVs and the empty data LVs for disposal by the caller */
	if ((seg->meta_areas && !_extract_image_component_list(seg, RAID_META,  0, removal_lvs)) ||
	    !_extract_image_component_list(seg, RAID_IMAGE, 0, removal_lvs))
		return_0;

	/*
	 * Remove the one segment holding the image component areas
	 * from the top-level LV, then add the new segments to it
	 */
	dm_list_del(&seg->list);
	dm_list_splice(&lv->segments, &new_segments);

	return 1;
}

/*
 * HM
 *
 * Helper convert raid0 to striped
 *
 * Convert a RAID0 set to striped
 *
 * Returns: 1 on success, 0 on failure
 *
 * HM FIXME: check last_seg(lv)->reshape_len and reduce LV aprropriately
 */
static int _convert_raid0_to_striped(struct logical_volume *lv,
				     int update_and_reload,
				     struct dm_list *removal_lvs)
{
	/* Caller should ensure, but... */
	if (!seg_is_any_raid0(first_seg(lv))) {
		log_error(INTERNAL_ERROR "Cannot convert non-%s LV %s to %s",
			  SEG_TYPE_NAME_RAID0, display_lvname(lv), SEG_TYPE_NAME_STRIPED);
		return 0;
	}

	/* Move the AREA_PV areas across to new top-level segments of type "striped" */
	if (!_raid0_to_striped_retrieve_segments_and_lvs(lv, removal_lvs)) {
		log_error("Failed to retrieve raid0 segments from %s.", lv->name);
		return_0;
	}

	lv->status &= ~RAID;

	if (!(first_seg(lv)->segtype = get_segtype_from_string(lv->vg->cmd, SEG_TYPE_NAME_STRIPED)))
		return_0;

	if (update_and_reload) {
		if (!lv_update_and_reload(lv))
			return_0;

		/* Eliminate the residual LVs, write VG, commit it and take a backup */
		return _eliminate_extracted_lvs(lv->vg, removal_lvs);
	}

	return 1;
}
/* END: raid0 -> striped conversion */

/* BEGIN: raid <-> raid conversion */
/*
 *
 * HM
 *
 * Compares current raid disk count of active RAID set @lv to
 * requested @dev_count returning number of disks as of healths
 * string in @devs_health and synced disks in @devs_in_sync
 *
 * Returns:
 *
 * 	0: error
 * 	1: kernel dev count = @dev_count
 * 	2: kernel dev count < @dev_count
 * 	3: kernel dev count > @dev_count
 *
 */
static int _reshaped_state(struct logical_volume *lv, const unsigned dev_count,
			   unsigned *devs_health, unsigned *devs_in_sync)
{
	unsigned d;
	uint32_t kernel_devs;
	char *raid_health;

	*devs_health = *devs_in_sync = 0;

	if (!lv_raid_dev_count(lv, &kernel_devs)) {
		log_error("Failed to get device count");
		return_0;
	}

	if (!lv_raid_dev_health(lv, &raid_health)) {
		log_error("Failed to get device health");
		return_0;
	}

	d = (unsigned) strlen(raid_health);
	while (d--) { 
		(*devs_health)++;
		if (raid_health[d] == 'A')
			(*devs_in_sync)++;
	}

PFLA("kernel_devs=%u dev_count=%u", kernel_devs, dev_count);
	if (kernel_devs == dev_count)
		return 1;

	return kernel_devs < dev_count ? 2 : 3;
}

/*
 * Reshape logical volume @lv by adding/removing stripes
 * (absolute new stripes given in @new_stripes), changing
 * layout (e.g. raid5_ls -> raid5_ra) or changing
 * stripe size to @new_stripe_size.
 *
 * In case of disk addition, any PVs listed in
 * @allocate_pvs will be used for allocation of
 * new stripes.
 */
static int _raid_reshape(struct logical_volume *lv,
			 const struct segment_type *new_segtype,
			 int yes, int force,
			 const unsigned new_stripes,
			 const unsigned new_stripe_size,
		 	 struct dm_list *allocate_pvs)
{
	int r;
	int too_few = 0;
	uint32_t new_len, s;
	struct lv_segment *seg = first_seg(lv);
	unsigned old_dev_count = seg->area_count;
	unsigned new_dev_count = new_stripes + seg->segtype->parity_devs;
	unsigned devs_health, devs_in_sync;
	struct dm_list removal_lvs;
	struct lvinfo info = { 0 };

	dm_list_init(&removal_lvs);

PFLA("old_dev_count=%u new_dev_count=%u", old_dev_count, new_dev_count);
	if (seg->segtype == new_segtype &&
	    old_dev_count == new_dev_count &&
	    seg->stripe_size == new_stripe_size) {
		log_error("Nothing to do");
		return 0;
	}

	if (!seg_is_raid4(seg) && !seg_is_any_raid5(seg) && !seg_is_any_raid6(seg) &&
	    !seg_is_raid10_near(seg) && !seg_is_raid10_offset(seg) &&
	    (old_dev_count != new_dev_count || seg->stripe_size != new_stripe_size)) {
		log_error("Can't reshape %s LV %s", seg->segtype->name, display_lvname(lv));
		log_error("You may want to convert to raid4/5/6/10_far/10_offset first");
		return 0;
	}

	if (seg_is_any_raid10(seg)) {
		if (seg_is_raid10_far(seg) ||
	    	segtype_is_raid10_far(new_segtype)) {
			log_error("Can't reshape any raid10_far LV %s", display_lvname(lv));
			log_error("You may want to use the \"--duplicate\" option");
			return 0;
		}

		if (((seg_is_raid10_near(seg) && segtype_is_raid10_offset(new_segtype)) ||
		     (seg_is_raid10_offset(seg) && segtype_is_raid10_near(new_segtype))) &&
		    new_dev_count != old_dev_count) {
			log_error("Can't reshape raid10 LV %s to different layout and "
				  "change number of disks at the same time", display_lvname(lv));
			log_error("You may want to use the \"--duplicate\" option");
			return 0;
		}
#if 0
		if (new_dev_count < old_dev_count) {
			log_error("Can't remove disks from raid10 LV %s", display_lvname(lv));
			log_error("You may want to use the \"--duplicate\" option");
			return 0;
		}
#endif
		if (new_dev_count < seg->data_copies) {
			log_error("Can't remove disks from raid10 LV %s and have less disks than data copies",
				  display_lvname(lv));
			return 0;
		}
	}

	/* raid4/5 with N image component pairs (i.e. N-1 stripes): allow for raid4/5 reshape to 2 devices, i.e. raid1 layout */
	if (seg_is_raid4(seg) || seg_is_any_raid5(seg)) {
		if (new_stripes < 1)
			too_few = 1;

	/* raid6 device count: check for 2 stripes minimum */
	} else if (new_stripes < 2)
		too_few = 1;

	if (too_few) {
		log_error("Too few stripes requested");
		return 0;
	}

	seg->stripe_size = new_stripe_size;
	switch ((r = _reshaped_state(lv, old_dev_count, &devs_health, &devs_in_sync))) {
	case 1:
		/*
		 * old_dev_count == kernel_dev_count
		 *
		 * Check for device health
		 */
		if (devs_in_sync < devs_health) {
			log_error("Can't reshape out of sync LV %s", display_lvname(lv));
			return 0;
		}
PFL();
		/* device count and health are good -> ready to add disks */
		break;

	case 2:
PFLA("devs_in_sync=%u old_dev_count=%u new_dev_count=%u", devs_in_sync,old_dev_count, new_dev_count);
		if (devs_in_sync == new_dev_count)
			break;

		/* Possible after a shrinking reshape and forgotten device removal */
		log_error("Device count is incorrect. "
			  "Forgotten \"lvconvert --stripes %d %s\" to remove %u images after reshape?",
			  devs_in_sync - seg->segtype->parity_devs, display_lvname(lv),
			  old_dev_count - devs_in_sync);
		return 0;

	default:
		log_error(INTERNAL_ERROR "Bad return=%d provided to %s.", r, __func__);
		return 0;
	}

	/* Handle disk addition reshaping */
	if (old_dev_count < new_dev_count) {
PFL();
		/* Conversion to raid1 */
		if (old_dev_count == 2)
			new_segtype = seg->segtype;

		if (!lv_info(lv->vg->cmd, lv, 0, &info, 1, 0) && driver_version(NULL, 0)) {
			log_error("lv_info failed: aborting");
			return 0;
		}

		new_len = _data_rimages_count(seg, new_dev_count) * (seg->len / _data_rimages_count(seg, old_dev_count));
		log_warn("WARNING: Adding stripes to active%s logical volume %s will grow "
			 "it from %u to %u extents!\n"
			 "You may want to run \"lvresize -l%u %s\" to shrink it after\n"
			 "the conversion has finished or make use of the gained capacity",
			 info.open_count ? " and open" : "",
			 display_lvname(lv), seg->len, new_len,
			 seg->len, display_lvname(lv));

		if (!_yes_no_conversion(lv, new_segtype, yes, force, new_dev_count, new_stripes, new_stripe_size))
			return 0;

		/* Allocate forward out of place reshape space at the beginning of all data image LVs */
		if (!_lv_alloc_reshape_space(lv, alloc_begin, allocate_pvs))
			return 0;

		/* Allocate new image component pairs for the additional stripes and grow LV size */
		log_debug_metadata("Addingg %u data and metadata image LV pair%s to %s",
				   new_dev_count - old_dev_count, new_dev_count - old_dev_count > 1 ? "s" : "",
				   display_lvname(lv));
		if (!_lv_change_image_count(lv, new_segtype, new_dev_count, allocate_pvs, NULL))
			return 0;

		if (seg->segtype != new_segtype)
			log_warn("Ignoring layout change on device adding reshape");

	/* Handle disk removal reshaping */
	} else if (old_dev_count > new_dev_count) {
		switch (_reshaped_state(lv, new_dev_count, &devs_health, &devs_in_sync)) {
		case 3:
			/*
			 * Disk removal reshape step 1:
			 *
			 * we got more disks active than requested via @new_stripes
			 *
			 * -> flag the ones to remove
			 *
			 */
PFL();
			if (!lv_info(lv->vg->cmd, lv, 0, &info, 1, 0) && driver_version(NULL, 0)) {
				log_error("lv_info failed: aborting");
				return 0;
			}

			new_len = _data_rimages_count(seg, new_dev_count) *
				  (seg->len / _data_rimages_count(seg, old_dev_count));
PFLA("new_dev_count=%u _data_rimages_count(seg, new_dev_count)=%u new_len=%u", new_dev_count, _data_rimages_count(seg, new_dev_count), new_len);
			log_warn("WARNING: Removing stripes from active%s logical volume %s will shrink "
				 "it from %s to %s!",
				 info.open_count ? " and open" : "", display_lvname(lv),
				 display_size(lv->vg->cmd, seg->len * lv->vg->extent_size), 
				 display_size(lv->vg->cmd, new_len * lv->vg->extent_size));
			log_warn("THIS MAY DESTROY (PARTS OF) YOUR DATA!");
			log_warn("You may want to interrupt the conversion and run \"lvresize -y -l%u %s\" ",
				 (uint32_t) ((uint64_t) seg->len * seg->len / new_len), display_lvname(lv));
			log_warn("to keep the current size if you haven't done it already");
			log_warn("If that leaves the logical volume larger than %u extents due to stripe rounding,",
				 new_len);
			log_warn("you may want to grow the content afterwards (filesystem etc.)");
			log_warn("WARNING: You have to run \"lvconvert --stripes %u %s\" again after the reshape has finished",
				 new_stripes, display_lvname(lv));
			log_warn("in order to remove the freed up stripes from the raid set");

			if (!_yes_no_conversion(lv, new_segtype, yes, force, new_dev_count, new_stripes, new_stripe_size))
				return 0;

			if (!force) {
				log_warn("WARNING: Can't remove stripes without --force option");
				return 0;
			}

			/*
			 * Allocate backward out of place reshape space at the
			 * _end_ of all data image LVs, because MD reshapes backwards
			 * to remove disks from a raid set
			 */
			if (!_lv_alloc_reshape_space(lv, alloc_end, allocate_pvs))
				return 0;

			for (s = new_dev_count; s < old_dev_count; s++)
				seg_lv(seg, s)->status |= LV_RESHAPE_DELTA_DISKS_MINUS;

			if (seg->segtype != new_segtype)
				log_warn("Ignoring layout change on device removing reshape");

			break;

		case 1:
			/*
		 	* Disk removal reshape step 2:
		 	*
		 	* we got the proper (smaller) amount of devices active
		 	* for a previously finished disk removal reshape
		 	*
		 	* -> remove the freed up images and reduce LV size
		 	*
		 	*/
PFL();
			log_debug_metadata("Removing %u data and metadata image LV pair%s from %s",
					   old_dev_count - new_dev_count, old_dev_count - new_dev_count > 1 ? "s" : "",
					   display_lvname(lv));
			if (!_lv_change_image_count(lv, new_segtype, new_dev_count, allocate_pvs, &removal_lvs))
				return 0;

			break;

		default:
PFL();
			log_error(INTERNAL_ERROR "Bad return provided to %s.", __func__);
			return 0;
		}

	/* Handle raid set layout reshaping (e.g. raid5_ls -> raid5_n) */
	} else {
PFL();
		if (!_yes_no_conversion(lv, new_segtype, yes, force, new_dev_count, new_stripes, new_stripe_size))
			return 0;
PFL();
		/*
		 * Reshape layout or chunksize:
		 *
		 * Allocate free out of place reshape space anywhere, thus
		 * avoiding remapping it in case it is already allocated.
		 *
		 * The dm-raid target is able to use the space whereever it
		 * is found by appropriately selecting forward or backward reshape.
		 */
		if (!_lv_alloc_reshape_space(lv, alloc_anywhere, allocate_pvs))
			return 0;

		seg->segtype = new_segtype;
	}

	/* HM FIXME: workaround for lvcreate not resetting "nosync" flag */
	init_mirror_in_sync(0);

PFLA("new_segtype=%s seg->area_count=%u", new_segtype->name, seg->area_count);

	return _lv_update_and_reload_origin_eliminate_lvs(lv, &removal_lvs);
}

/*
 * Check for reshape request defined by:
 *
 * - raid type is reshape capable
 * - no raid level change
 * - # of stripes requested to change
 *   (i.e. add/remove disks from a striped raid set)
 *   -or-
 * - stripe size change requestd
 *   (e.g. 32K -> 128K)
 */
static int _reshape_requested(const struct logical_volume *lv, const struct segment_type *segtype,
			      const uint32_t stripes, const uint32_t stripe_size)
{
	struct lv_segment *seg = first_seg(lv);

PFL();
	if (!seg_is_reshapable_raid(seg))
		return 0;
PFL();
	if (!is_same_level(seg->segtype, segtype))
		return 0;

	/* Special raid10_{near,offset} case */
	if ((seg_is_raid10_near(seg) && segtype_is_raid10_offset(segtype)) ||
	    (seg_is_raid10_offset(seg) && segtype_is_raid10_near(segtype)))
		return 1;
PFL();
	if (seg_is_raid10_far(seg)) {
		log_error("Can't reshape raid10_far LV %s.", display_lvname(lv));
		log_error("Use \"lvconvert --duplicate ... %s", display_lvname(lv));
		return 2;
	}

	if (stripes && stripes == _data_rimages_count(seg, seg->area_count)) {
		log_error("LV %s already has that amount of stripes.", display_lvname(lv));
		return 2;
	}

PFL();
	if (stripe_size && stripe_size == seg->stripe_size) {
		log_error("LV %s already has that stripe size.", display_lvname(lv));
		return 2;
	}

	if (!stripes && !stripe_size)
		return 0;

	return 1;
}


/*
 * HM
 *
 * TAKEOVER: copes with all raid level switches aka takeover of @lv
 *
 * Overwrites the users "--type level_algorithm" request (e.g. --type raid6_zr)
 * with the appropriate, constrained one to allow for takeover (e.g. raid6_n_6).
 *
 * raid0 can take over:
 *  raid4
 *  raid5
 *  raid10 - assuming we have all necessary active disks
 *  raid1
 *
 * raid1 can take over:
 *  raid5 with 2 devices, any layout or chunk size
 *
 * raid10 can take over:
 *  raid0 - with any number of drives
 *
 * raid4 can take over:
 *  raid0 - if there is only one strip zone
 *  raid5 - if layout is right (parity on last disk)
 *
 * raid5 can take over:
 *  raid0 - if there is only one strip zone - make it a raid4 layout
 *  raid1 - if there are two drives.  We need to know the chunk size
 *  raid4 - trivial - just use a raid4 layout.
 *  raid6 - Providing it is a *_6 layout
 *
 * raid6 currently can only take over a (raid4/)raid5.  We map the
 * personality to an equivalent raid6 personality
 * with the Q block at the end.
 */
struct possible_type {
	const uint64_t current_types;
	const uint64_t possible_types;
};
/*
 * Return any segtype flags it is possible to convert to from @seg 
 *
 * HM FIXME: complete?
 */
static uint64_t _get_conversion_to_segtypes(const struct lv_segment *seg)
{
	unsigned cn;
	struct possible_type pt[] = {
		{ .current_types  = SEG_AREAS_STRIPED, /* linear, i.e. seg->area_count = 1 */
		  .possible_types = SEG_RAID1|SEG_RAID10|SEG_RAID4|SEG_RAID5_LS|SEG_RAID5_LA| \
				    SEG_RAID5_RS|SEG_RAID5_RA|SEG_RAID5_N },
		{ .current_types  = SEG_AREAS_STRIPED, /* striped, i.e. seg->area_count > 1 */
		  .possible_types = SEG_RAID0|SEG_RAID0_META|SEG_RAID01|SEG_RAID10|SEG_RAID4|SEG_RAID5_N|SEG_RAID6_N_6 },
		{ .current_types  = SEG_RAID0|SEG_RAID0_META, /* seg->area_count = 1 */
		  .possible_types = SEG_RAID1|SEG_RAID10|SEG_RAID4|SEG_RAID5_LS|SEG_RAID5_LA| \
				    SEG_RAID5_RS|SEG_RAID5_RA|SEG_RAID5_N },
		{ .current_types  = SEG_RAID0|SEG_RAID0_META, /* raid0 striped, i.e. seg->area_count > 1 */
		  .possible_types = SEG_AREAS_STRIPED|SEG_RAID10|SEG_RAID4|SEG_RAID5_N|SEG_RAID6_N_6 },
		{ .current_types  = SEG_RAID1, /* only if seg->area_count = 2 */
		  .possible_types = SEG_RAID10|SEG_RAID4|SEG_RAID5_LS|SEG_RAID5_LA|SEG_RAID5_RS|SEG_RAID5_RA|SEG_RAID5_N },
		{ .current_types  = SEG_RAID4,
		  .possible_types = SEG_AREAS_STRIPED|SEG_RAID0|SEG_RAID0_META|SEG_RAID5_N|SEG_RAID6_N_6 },
		{ .current_types  = SEG_RAID5_LS,
		  .possible_types = SEG_RAID5_N|SEG_RAID5_LA|SEG_RAID5_RS|SEG_RAID5_RA|SEG_RAID6_LS_6 },
		{ .current_types  = SEG_RAID5_LA,
		  .possible_types = SEG_RAID5|SEG_RAID5_N|SEG_RAID5_LS|SEG_RAID5_RS|SEG_RAID5_RA|SEG_RAID6_LA_6 },
		{ .current_types  = SEG_RAID5_RS,
		  .possible_types = SEG_RAID5|SEG_RAID5_N|SEG_RAID5_LS|SEG_RAID5_LA| SEG_RAID5_RA|SEG_RAID6_RS_6 },
		{ .current_types  = SEG_RAID5_RA,
		  .possible_types = SEG_RAID5|SEG_RAID5_N|SEG_RAID5_LS|SEG_RAID5_RS|SEG_RAID5_LA|SEG_RAID6_RA_6 },
		{ .current_types  = SEG_RAID5_N,
		  .possible_types = SEG_AREAS_STRIPED|SEG_RAID0|SEG_RAID0_META|SEG_RAID4| \
				    SEG_RAID5_LA|SEG_RAID5_LS|SEG_RAID5_RS|SEG_RAID5_RA|SEG_RAID6_N_6 },
		{ .current_types  = SEG_RAID6_NC|SEG_RAID6_NR|SEG_RAID6_ZR,
		  .possible_types = SEG_RAID6_NC|SEG_RAID6_NR|SEG_RAID6_ZR|SEG_RAID6_N_6 },
		{ .current_types  = SEG_RAID6_LS_6,
		  .possible_types = SEG_RAID6_LA_6|SEG_RAID6_RS_6|SEG_RAID6_RA_6| \
				    SEG_RAID6_NC|SEG_RAID6_NR|SEG_RAID6_ZR|SEG_RAID6_N_6|SEG_RAID5_LS },
		{ .current_types  = SEG_RAID6_RS_6,
		  .possible_types = SEG_RAID6_LS_6|SEG_RAID6_LA_6|SEG_RAID6_RA_6| \
				    SEG_RAID6_NC|SEG_RAID6_NR|SEG_RAID6_ZR|SEG_RAID6_N_6|SEG_RAID5_RS },
		{ .current_types  = SEG_RAID6_LA_6,
		  .possible_types = SEG_RAID6_LS_6|SEG_RAID6_RS_6|SEG_RAID6_RA_6| \
				    SEG_RAID6_NC|SEG_RAID6_NR|SEG_RAID6_ZR|SEG_RAID6_N_6|SEG_RAID5_LA },
		{ .current_types  = SEG_RAID6_RA_6,
		  .possible_types = SEG_RAID6_LS_6|SEG_RAID6_LA_6|SEG_RAID6_RS_6| \
				    SEG_RAID6_NC|SEG_RAID6_NR|SEG_RAID6_ZR|SEG_RAID6_N_6|SEG_RAID5_RA },
		{ .current_types  = SEG_RAID6_N_6,
		  .possible_types = SEG_RAID6_LS_6|SEG_RAID6_LA_6|SEG_RAID6_RS_6|SEG_RAID6_RA_6| \
				    SEG_RAID6_NR|SEG_RAID6_NC|SEG_RAID6_ZR| \
				    SEG_RAID5_N|SEG_RAID4|SEG_RAID0|SEG_RAID0_META|SEG_AREAS_STRIPED },
		{ .current_types  = SEG_RAID10,
		  .possible_types = SEG_AREAS_STRIPED|SEG_RAID0|SEG_RAID0_META }
	};

	for (cn = 0; cn < ARRAY_SIZE(pt); cn++)
		if (seg->segtype->flags & pt[cn].current_types) {
			/* Skip to striped (raid0), i.e. seg->rea_count > 1 */
			if ((seg_is_striped(seg) || seg_is_any_raid0(seg)) &&
			    seg->area_count > 1 && (SEG_RAID1 & pt[cn].possible_types))
				continue;

			return pt[cn].possible_types;
		}

	return 0;
}

/*
 * Log any possible conversions for @lv
 */
/* HM FIXEM: use log_info? */
static void _log_possible_conversion_types(struct logical_volume *lv)
{
	unsigned i;
	uint64_t t, possible_types;
	const char *alias = "";
	const struct lv_segment *seg = first_seg(lv);
	const struct segment_type *segtype;

	if (!(possible_types = _get_conversion_to_segtypes(seg)))
		return;

	if (seg_is_raid1(seg) && seg->area_count != 2)
		log_warn("Conversions on raid1 LV %s only possible after \"lvconvert -m1 %s\"",
			 display_lvname(lv), display_lvname(lv));

	if (seg_is_raid5_ls(seg))
		alias = SEG_TYPE_NAME_RAID5_LS;
	else if (seg_is_raid6_zr(seg))
		alias = SEG_TYPE_NAME_RAID6_ZR;
	else if (seg_is_any_raid10(seg) && !seg_is_raid10_near(seg))
		alias = SEG_TYPE_NAME_RAID10;

	log_warn("Converting %s from %s%s%s%c without \"--duplicate\" is possible to:",
		 display_lvname(lv), _get_segtype_name(seg->segtype, seg->area_count),
		 *alias ? " (same as " : "", alias, *alias ? ')' : 0);

	/* Print any possible segment types to convert to */
	for (i = 0; i < 64; i++) {
		t = 1ULL << i;
		if ((t & possible_types) &&
		    ((segtype = get_segtype_from_flag(lv->vg->cmd, t))))
			log_warn("%s", segtype->name);
	}
}

/*
 * Find takeover raid flag for segment type flag of @seg
 */
/* Segment type flag correspondence raid5 <-> raid6 conversions */
static uint64_t _r5_to_r6[][2] = {
	{ SEG_RAID5_LS, SEG_RAID6_LS_6 },
	{ SEG_RAID5_LA, SEG_RAID6_LA_6 },
	{ SEG_RAID5_RS, SEG_RAID6_RS_6 },
	{ SEG_RAID5_RA, SEG_RAID6_RA_6 },
	{ SEG_RAID5_N,  SEG_RAID6_N_6 },
};


/* Return segment type flag for raid5 -> raid6 conversions */
static uint64_t _get_r56_flag(const struct lv_segment *seg, unsigned idx1, unsigned idx2)
{
	unsigned elems = ARRAY_SIZE(_r5_to_r6);

	while (elems--)
		if (seg->segtype->flags & _r5_to_r6[elems][idx1])
			return _r5_to_r6[elems][idx2];

	return 0;
}

/* Return segment type flag for raid5 -> raid6 conversions */
static uint64_t _raid_seg_flag_5_to_6(const struct lv_segment *seg)
{
	return _get_r56_flag(seg, 0, 1);
}

/* Return segment type flag for raid6 -> raid5 conversions */
static uint64_t _raid_seg_flag_6_to_5(const struct lv_segment *seg)
{
	return _get_r56_flag(seg, 1, 0);
}
/******* END: raid <-> raid conversion *******/



/****************************************************************************/
/****************************************************************************/
/****************************************************************************/
/* Construction site of takeover handler function jump table solution */

/*
 * Update metadata, reload origin @lv, eliminate any LVs listed on @remova_lvs
 * and then clear flags passed to the kernel (if any) in the metadata
 */
static int _lv_update_and_reload_origin_eliminate_lvs(struct logical_volume *lv,
						      struct dm_list *removal_lvs)
{
	int flag_cleared;

	log_debug_metadata("Updating metadata and reloading mappings for %s,",
			   display_lvname(lv));

PFL();
	if (!lv_update_and_reload_origin(lv))
		return_0;

	/* Eliminate any residual LV, write VG, commit it and take a backup */
	if (!_eliminate_extracted_lvs(lv->vg, removal_lvs))
		return_0;

	/*
	 * Now that any 'REBUILD' or 'RESHAPE_DELTA_DISKS' etc. has/have made
	 * its/their way to the kernel, we must remove the flag(s) so that the
	 * individual devices are not rebuilt/reshaped upon every activation.
	 */
	log_debug_metadata("Clearing any flags for %s passed to the kernel.",
			   display_lvname(lv));
PFL();
	if (!_reset_flags_passed_to_kernel(lv, &flag_cleared))
		return 0;
PFL();
	return flag_cleared ? lv_update_and_reload_origin(lv) : 1;
}

/* Display error message and return 0 if @lv is not synced, else 1 */
static int _lv_is_synced(struct logical_volume *lv)
{
	if (lv->status & LV_NOTSYNCED) {
		log_error("Can't convert out-of-sync LV %s"
			  " use 'lvchange --resync %s' first",
			  display_lvname(lv), display_lvname(lv));
		return 0;
	}

	return 1;
}

/* Begin: various conversions between layers (aka MD takeover) */
/*
 * takeover function argument list definition
 *
 * All takeover functions and helper functions
 * to support them have this list of arguments
 */
#define TAKEOVER_FN_ARGUMENTS			\
	struct logical_volume *lv,		\
	const struct segment_type *new_segtype,	\
	int yes, int force,			\
	unsigned new_image_count,		\
	const unsigned new_data_copies,		\
	const unsigned new_stripes,		\
	unsigned new_stripe_size,		\
	struct dm_list *allocate_pvs

#if 0
	unsigned new_region_size,
	unsigned new_extents,
#endif
/*
 * a matrix with types from -> types to holds
 * takeover function pointers this prototype
 */
typedef int (*takeover_fn_t)(TAKEOVER_FN_ARGUMENTS);

/* Return takeover function table index for @segtype */
static unsigned _takeover_fn_idx(const struct segment_type *segtype, uint32_t area_count)
{
	static uint64_t _segtype_to_idx[] = {
		0, /* linear, seg->area_count = 1 */
		SEG_AREAS_STRIPED,
		SEG_MIRROR,
		SEG_RAID0,
		SEG_RAID0_META,
		SEG_RAID1,
		SEG_RAID4|SEG_RAID5_LS|SEG_RAID5_LA|SEG_RAID5_LS|SEG_RAID5_RS|SEG_RAID5_RA|SEG_RAID5_N,
		SEG_RAID6_LS_6|SEG_RAID6_LA_6|SEG_RAID6_RS_6|SEG_RAID6_RA_6| \
		SEG_RAID6_NC|SEG_RAID6_NR|SEG_RAID6_ZR|SEG_RAID6_N_6,
		SEG_RAID10_NEAR|SEG_RAID10_FAR|SEG_RAID10_OFFSET,
		SEG_RAID01
	};
	unsigned r = ARRAY_SIZE(_segtype_to_idx);

PFLA("segtype=%s area_count=%u", segtype->name, area_count);
	/* Linear special case */
	if (segtype_is_striped(segtype) && area_count == 1)
		return 0;

	while (r-- > 0)
		if (segtype->flags & _segtype_to_idx[r])
{
PFLA("r=%u", r);
			return r;
}

	return 0;
}

/* Macro to define raid takeover helper function header */
#define TAKEOVER_FN(function_name) \
static int function_name(TAKEOVER_FN_ARGUMENTS)

/* Macro to spot takeover helper functions easily */
#define TAKEOVER_HELPER_FN(function_name) TAKEOVER_FN(function_name)
#define TAKEOVER_HELPER_FN_REMOVAL_LVS(function_name) \
static int function_name(TAKEOVER_FN_ARGUMENTS, struct dm_list *removal_lvs)

/*
 * noop and error takoover handler functions
 * to allow for logging that an lv already
 * has the requested type or that the requested
 * conversion is not possible
 */
/* Noop takeover handler for @lv: logs that LV already is of the requested type */
TAKEOVER_FN(_noop)
{
	log_warn("Logical volume %s already is of requested type %s",
		 display_lvname(lv), lvseg_name(first_seg(lv)));

	return 0;
}

/* Error takeover handler for @lv: logs what's (im)possible to convert to (and mabye added later) */
TAKEOVER_FN(_error)
{
	struct lv_segment *seg = first_seg(lv);

	log_error("Converting the segment type for %s (directly) from %s to %s"
		  " is not supported (yet).", display_lvname(lv),
		  lvseg_name(seg), new_segtype->name);
	log_error("You may want to use the \"--duplicate\" option");

	return 0;
}

/****************************************************************************/
/*
 * HM FIXME: Bastelkeller ;-)
 *
 * Conversion via creation of a new LV to put
 * top-level raid1 on top of source and destination
 */
/* Create a new LV with type @segtype */
static struct logical_volume *_lv_create(struct volume_group *vg, const char *lv_name,
					 const struct segment_type *segtype,
					 uint32_t data_copies, uint32_t stripes,
					 uint32_t region_size, uint32_t stripe_size,
					 uint32_t extents, struct dm_list *pvs)
{
	struct logical_volume *r;
	struct lvcreate_params lp = {
		.activate = CHANGE_ALY, /* Active to avoid "unsafe table load" for destination LV */
		.alloc = ALLOC_INHERIT,
		.extents = extents,
		.major = -1,
		.minor = -1,
		.log_count = 0,
		.lv_name = lv_name,
		.mirrors = data_copies,
		.nosync = 1,
		.permission = LVM_READ | LVM_WRITE,
		.pvh = pvs ? : &vg->pvs,
		.read_ahead = DM_READ_AHEAD_AUTO,
		.region_size = region_size,
		.segtype = segtype,
		.stripes = stripes,
		.stripe_size = stripe_size,
		.tags = DM_LIST_HEAD_INIT(lp.tags),
		.temporary = 0,
		.zero = 0,
	};

PFLA("lv_name=%s segtype=%s data_copies=%u stripes=%u region_size=%u stripe_size=%u extents=%u",
     lv_name, segtype->name, data_copies, stripes, region_size, stripe_size, extents);

	if (segtype_is_striped_raid(segtype) && stripes < 2) {
		log_warn("Adjusting stripes to the minimum of 2");
		lp.stripes = 2;
	}

	if (segtype_is_any_raid10(segtype)) {
		if (data_copies < 2)
			lp.mirrors = 2;

		if (data_copies > stripes) {
			log_error("raid10 data_copies may not be more than stripes (i.e. -mN with N <= #stripes)");
			return_NULL;
		}

	} else if (segtype_is_mirror(segtype)) {
PFLA("segtype->parity_devs=%u", segtype->parity_devs);
		lp.mirrors = data_copies > 1 ? data_copies : 2;
		lp.log_count = 1;
PFLA("data_copies=%u", data_copies);
		lp.stripes = 1;
		lp.stripe_size = 0;
	}

	/* FIXME: Maybe using silent mode ? */
	log_verbose("Preparing pool metadata spare volume for Volume group %s.", vg->name);
	if (!(r = lv_create_single(vg, &lp)))
		return_NULL;

	if (segtype_is_mirror(segtype))
		first_seg(r)->meta_areas = NULL;

	return r;
}

/* Helper: Create a unique name from @lv->name and string @(suffix + 1) adding a number */
static char *_unique_lv_name(struct logical_volume *lv, const char *suffix)
{
	char *name;
	uint32_t count = 0;

	/* Loop until we found an available one */
	while (!(name = _generate_raid_name(lv, suffix + 1, count)))
		count++;

	if (!name)
		log_error("Failed to create unique sub-lv name for %s", display_lvname(lv));

	return name;
}

static void _rename_lv(struct logical_volume *lv, const char *from, const char *to)
{
	size_t sz;
	char *name = (char *) lv->name, *p;

	if (!(p = strstr(lv->name, from))) {
		log_error(INTERNAL_ERROR "Failed to find %s in lv name %s", from, display_lvname(lv));
		return;
	}

	sz = p - lv->name + strlen(to) + (strlen(p) - strlen(from)) + 1;
	if (!(name = dm_pool_alloc(lv->vg->vgmem, sz))) {
		log_error(INTERNAL_ERROR "Failed to allocate name for %s", display_lvname(lv));
		return;
	}
		
	sz = p - lv->name;
	strncpy(name, lv->name, sz);
	strncpy(name + sz, to, strlen(to));
	strcpy(name + sz + strlen(to), p + strlen(from));

	lv->name = name;
}

/*
 * Helper to rename rimage/rmeta or mimage/mlog name
 * suffixes to/from duplication conversion namespace
 */
/* HM FIXME: enhance to support "mirror" log LV(s) */
static void __rename_sub_lvs(struct logical_volume *lv, int to_dup, uint64_t flags)
{
	uint32_t s;
	struct lv_segment *seg = first_seg(lv);
	struct from_to {
		const char *image[2];
		const char *meta[2];
	};
	static struct from_to ft_raid = {
		{ "_rimage", "_rdimage" },
		{ "_rmeta" , "_rdmeta"  }
	};
	static struct from_to ft_mirror = {
		{ "_mimage", "_mdimage" },
		{ "_mlog" ,  "_mdlog"  }
	};
	struct from_to *ft = seg_is_mirror(seg) ? &ft_mirror : &ft_raid;

	for (s = 0; s < seg->area_count; s++) {
		if (seg_type(seg, s) == AREA_LV && (flags & RAID_IMAGE))
			_rename_lv(seg_lv(seg, s), ft->image[!to_dup], ft->image[!!to_dup]);

		if (seg->meta_areas && (flags & RAID_META))
			_rename_lv(seg_metalv(seg, s), ft->meta[!to_dup], ft->meta[!!to_dup]);
	}

	if (seg->log_lv)
		_rename_lv(seg->log_lv, ft->meta[!to_dup], ft->meta[!!to_dup]);
}

static void _rename_sub_lvs(struct logical_volume *lv, int to_dup)
{
	return __rename_sub_lvs(lv, to_dup, RAID_IMAGE | RAID_META);
}

static void _rename_metasub_lvs(struct logical_volume *lv, int to_dup)
{
	return __rename_sub_lvs(lv, to_dup, RAID_META);
}

/*
 * HM Helper:
 *
 * remove top-level raid1 lv with either source/destination
 * legs selected by --type/--stripes/--mirrors arguments option
 */
static int _raid_conv_unduplicate(struct logical_volume *lv,	
				  const struct segment_type *new_segtype,
				  unsigned new_image_count,
				  unsigned new_data_copies,
				  int yes)
{
	/*
	 * If we get here and the top-level raid1 is still synchronizing ->
	 *
	 * withdraw the destination LV thus canceling the conversion duplication,
	 * else withdraw the source LV
	 */
	uint32_t idx;
	struct dm_list removal_lvs;
	struct logical_volume *lv_tmp;
	struct lv_segment *seg = first_seg(lv);
	struct lv_segment *seg0 = first_seg(seg_lv(seg, 0));
	struct lv_segment *seg1 = first_seg(seg_lv(seg, 1));

	if (seg0->segtype == seg1->segtype) {
		if (seg0->area_count == seg1->area_count)
			idx = (seg1->data_copies == new_data_copies) ? 1 : 0;
		else
			idx = (seg1->area_count == new_image_count) ? 0 : 1;
	} else if (seg0->segtype == new_segtype)
		/* Keep source requested */
		idx = 1;
	else if (seg1->segtype == new_segtype)
		/* Keep destination requested */
		idx = 0;
	else {
		log_error("Wrong raid type %s requested to remove duplicating conversion",
			  new_segtype->name);
		log_error("Possible are either %s to keep the source or %s for the destination",
			  _get_segtype_name(seg0->segtype, new_image_count),
			  _get_segtype_name(seg1->segtype, new_image_count));
		return 0;
	}

	/* Removing the source requires the destination to be fully in sync! */
	if (!idx && !_raid_in_sync(lv)) {
		log_error("Can't convert to destination when LV %s is not in sync",
			  display_lvname(lv));
		return 0;
	}

	log_warn("This is a request to remove the %s of a duplicating conversion of LV %s!",
		 idx ? "destination" : "source", display_lvname(lv));
	if (!yes &&
	    yes_no_prompt("Do you want to convert %s with type %s to %s? [y/n]: ",
			  display_lvname(lv),
			  _get_segtype_name(seg0->segtype, seg0->area_count),
			  _get_segtype_name(seg1->segtype, new_image_count)) == 'n')
		return 0;

	if (idx)
		log_warn("Keeping source lv %s", display_lvname(seg_lv(seg, 0)));

	_rename_sub_lvs(seg_lv(seg, idx), 0);

	/*
	 * Extract image components of the raid1 top-level LV and set to error segment
	 * but the source/destination lv extracted, so that the sublvs get deactivated
	 */
	dm_list_init(&removal_lvs);
	if (!_extract_image_component_sublist(seg, RAID_META, 0, 1, &removal_lvs, 1) ||
	    !_extract_image_component_sublist(seg, RAID_META, 1, 2, &removal_lvs, 1) ||
	    !_extract_image_component_sublist(seg, RAID_IMAGE, idx, idx + 1, &removal_lvs, 0)) {
		log_error(INTERNAL_ERROR "Failed to extract top-level %s LVs %s images",
			  idx ? "source" : "destination",
			  display_lvname(seg_lv(seg, idx)));
		return 0;
	}

	/* If we drop source, move destination areas across */
	if (!idx) {
		seg->areas[0] = seg->areas[1];
		seg->meta_areas[0] = seg->meta_areas[1];
	}
	seg->area_count = 1;

	/* Add source/destination last image lv to removal_lvs */
	lv_tmp = seg_lv(seg, 0);
	if (!_lv_reset_raid_add_to_list(lv_tmp, &removal_lvs))
		return 0;

	/* Adjust size, because the 2 legs may differ caused by stripe rounding */
	lv->le_count = max(lv->le_count, lv_tmp->le_count);
	lv->size = lv->le_count * lv->vg->extent_size;

	/* Remove the raid1 layer from the LV */
	if (!remove_layer_from_lv(lv, lv_tmp))
		return_0;

	_rename_sub_lvs(lv, 0);
	lv_set_visible(lv);

	return _lv_update_and_reload_origin_eliminate_lvs(lv, &removal_lvs);
}

/*
 * Helper: raid to raid conversion by duplication
 *
 * Inserts a layer on top of the given @lv,
 * creates and allocates a destination lv of the
 * same size with the requested @new_segtype
 * and properties (e.g. stripes).
 */
TAKEOVER_HELPER_FN(_raid_conv_duplicate)
{
	uint32_t extents, region_size = 1024, s;
	char *lv_name, *p, *suffix;
	struct logical_volume *dst_lv;
	struct lv_segment *seg = first_seg(lv);
#if 1
	const char *lv_name_sav[2];
#endif

	/*
	 * In case of active conversion duplication, remove top-level raid1 lv with either
	 * source/destination legs selected by --type/--mirrors/--stripes argument options
	 */
	if (_lv_is_duplicating(lv))
		return _raid_conv_unduplicate(lv, new_segtype, new_image_count, new_data_copies, yes);

	/*
	 * Create duplicating LV by ceating a new LV with the
	 * requested properties and putting a raid1 on top
	 */
PFLA("new_segtype=%s new_data_copies=%u new_stripes=%u new_image_count=%u new_stripe_size=%u", new_segtype->name, new_data_copies, new_stripes, new_image_count, new_stripe_size);
PFLA("segtype=%s area_count=%u data_copies=%u stripe_size=%u", seg->segtype->name, seg->area_count, seg->data_copies, seg->stripe_size);
	new_stripe_size = new_stripe_size ?: seg->stripe_size;

	if (new_segtype == seg->segtype &&
	    new_stripes == seg->area_count &&
	    new_data_copies == seg->data_copies &&
	    new_stripe_size == seg->stripe_size) {
		log_error("No change on duplicating conversion of LV %s", display_lvname(lv));
		return 0;
	}

	if (seg_is_linear(seg) &&
	    (new_image_count > 1 || new_data_copies > 1 || new_stripes > 1)) {
		log_error("No stripes/mirrors with %s", new_segtype->name);
		return 0;
	}

	log_warn("This is a conversion by duplication request for source LV %s!", display_lvname(lv));
	log_warn("A new %s destination LV will be allocated and %s will be synced to it.",
		_get_segtype_name(new_segtype, new_image_count), display_lvname(lv));
	log_warn("You can either remove the source LV via 'lvconvert --type %s %s' after the synchronization finished",
		_get_segtype_name(seg->segtype, seg->area_count), display_lvname(lv));
	log_warn("or the destination LV via 'lvconvert --type %s %s' at any point in time (even during synchronization).",
		 _get_segtype_name(new_segtype, new_image_count), display_lvname(lv));
	if (!_yes_no_conversion(lv, new_segtype, yes, force, new_image_count, 0, 0))
		return 0;

	/*
	 * Creation of destination LV with intended layout and insertion of raid1 top-layer from here on
	 */
	new_image_count = new_image_count <= new_segtype->parity_devs ? 2 + new_segtype->parity_devs : new_image_count;
	new_stripe_size = new_stripe_size ?: 64 * 2;
	extents = lv->le_count - seg->reshape_len;

PFLA("new_image_count=%u extents=%u", new_image_count, extents);

	/*
	 * By default, prevent any PVs holding image components from
	 * being used for allocation unless --force provided
	 *
	 * HM FIXME: If non-redundant source/destination given/requested -> set force?
	 */
	if (!force) {
		log_debug_metadata("Avoiding coallocation  on source LV %s PVs", display_lvname(lv));
		if (!_avoid_pvs_with_other_images_of_lv(lv, allocate_pvs)) {
			log_error("Failed to prevent PVs holding image components "
				  "of source lv %s from being used for allocation.",
				  display_lvname(lv));
			return 0;
		}
	}

	log_debug_metadata("Creating unique LV name for source sub LV");
	if (!(lv_name = _unique_lv_name(lv, "_dsrc")))
		return 0;

	if (!(suffix = strstr(lv_name, "_dsrc"))) {
		log_error(INTERNAL_ERROR "Failed to find source prefix in source lv name %s", lv_name);
		return 0;
	}

	log_debug_metadata("Inserting layer lv on top of source LV %s", display_lvname(lv));
	/* seg after _convert_lv_to_raid() is the first segment of the new raid1 top-level lv */
	if (!(seg = _convert_lv_to_raid1(lv, suffix)))
		return 0;

	/* Rename soure lvs sub lvs */
	log_debug_metadata("Renaming source LV %s sub LVs", display_lvname(seg_lv(seg, 0)));
	_rename_sub_lvs(seg_lv(seg, 0), 1);

PFLA("seg->area_count=%u", seg->area_count);
PFLA("lv->name=%s lv->le_count=%u seg_lv(seg, 0)=%s", lv->name, lv->le_count, seg_lv(seg, 0)->name);

	log_debug_metadata("Creating unique LV name for destination sub LV");
	if (!(lv_name = _unique_lv_name(lv, "_ddst")))
		return 0;

	/* Create the destination lv */
	log_debug_metadata("Creating destination sub LV");
	if (!(dst_lv = _lv_create(lv->vg, lv_name, new_segtype, new_data_copies, new_stripes,
				  region_size, new_stripe_size, extents, allocate_pvs))) {
		log_error("Failed to create destination lv %s/%s", lv->vg->name, lv_name);
		return 0;
	}
	lv_set_hidden(dst_lv);
	dst_lv->status |= RAID_IMAGE;

	if (lv->le_count != dst_lv->le_count) {
		log_warn("Destination LV with %u extents is larger than source with %u due to stripe boundary rounding",
			 dst_lv->le_count, lv->le_count);
		log_warn("You may want to resize your LV content after the duplication conversion (e.g. resize fs)");
	}

PFL();
	/* Rename destination lvs sub lvs */
	log_debug_metadata("Renaming destination LV %s sub LVs", display_lvname(dst_lv));
	_rename_sub_lvs(dst_lv, 1);

	/* Grow areas arrays for data and metadata devs to add destination lv */
	log_debug_metadata("Realocating areas array of %s", display_lvname(lv));
	if (!_realloc_meta_and_data_seg_areas(lv, 2)) {
		log_error("Relocation of areas array for %s failed", display_lvname(lv));
		return_0;
	}

	/* Must update area count after resizing it */
	seg->area_count = 2;
PFL();
	log_debug_metadata("Add destination LV %s to top-level LV %s as second raid1 leg",
			   display_lvname(dst_lv), display_lvname(lv));
	/* Set @layer_lv as the lv of @area of @lv */
	if (!set_lv_segment_area_lv(seg, 1, dst_lv, dst_lv->le_count, dst_lv->status)) {
		log_error("Failed to add destination sublv %s to %s",
			  display_lvname(dst_lv), display_lvname(lv));
		return 0;
	}
#if 1
	/*
	 * Rename top-level raid1 sub LVs temporarily to create
	 * metadata sub LVs with "_rmeta" names.
	 *
	 * Need double '_' to not collide with dst_lv namespace
	 */
	for (s = 0; s < 2; s++) {
		lv_name_sav[s] = seg_lv(seg, s)->name;
		if (!(seg_lv(seg, s)->name = _generate_raid_name(lv, "_rimage", s)))
			return_0;
	}
#endif
PFLA("lv->name=%s meta_areas=%p", lv->name, seg->meta_areas);
	/* Enforce metadata image creation for top-level raid1 */
	seg->meta_areas = NULL;
	if (!_alloc_and_add_rmeta_devs_for_lv(lv))
		return 0;
#if 1
	/* Rename top-level raid1 sub LVs back */
	for (s = 0; s < 2; s++) {
		if ((p = strstr(seg_metalv(seg, s)->name, "__")))
			strcpy(p + 1, p + 2);

		seg_lv(seg, s)->name = lv_name_sav[s];
PFLA("seg_lv(seg, %u)=%s", s, seg_lv(seg, s)->name);
PFLA("seg_metalv(seg, %u)=%s", s, seg_metalv(seg, s)->name);
	}

	_rename_metasub_lvs(lv, 1);

	for (s = 0; s < 2; s++) {
PFLA("seg_lv(seg, %u)=%s", s, seg_lv(seg, s)->name);
PFLA("seg_metalv(seg, %u)=%s", s, seg_metalv(seg, s)->name);
	}
#endif
	/* Enforce destination lv to be synced */
	seg_lv(seg, 0)->status &= ~LV_REBUILD;
	seg_lv(seg, 1)->status |= LV_REBUILD;

	/* Set top-level LV status */
	lv->status |= RAID | RAID_IMAGE;
	lv->status &= ~LV_NOTSYNCED;
	lv_set_visible(lv);

PFLA("lv0->le_count=%u lv1->le_count=%u", seg_lv(seg, 0)->le_count, seg_lv(seg, 1)->le_count);

	/*
	 * Prevent dst_lv from syncing, because the top-level
	 * raid1 will rewrite it anyway during full resync
	 */
	init_mirror_in_sync(0);

	if (!_lv_update_and_reload_origin_eliminate_lvs(lv, NULL))
		return_0;

	/* HM FIXME: have to send message to correctly start rebuilding or it stops even w/o init_mirror_in_sync(0) ? */ 
	return _lv_cond_repair(lv);
}

/*
 * Begin takeover helper funtions
 */
/* Helper: linear -> raid0* */
TAKEOVER_HELPER_FN(_linear_raid0)
{
	struct lv_segment *seg = first_seg(lv);
	struct dm_list meta_lvs;

	dm_list_init(&meta_lvs);

	if ((!seg_is_linear(seg) && !seg_is_any_raid0(seg)) || new_image_count != 1) {
		log_error(INTERNAL_ERROR "Can't convert non-(linear|raid0) lv or to image count != 1");
		return 0;
	}

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	/* Convert any linear segment to raid1 by inserting a layer and presetting segtype as raid1 */
	if (seg_is_linear(seg)) {
		log_debug_metadata("Converting logical volume %s to raid",
				   display_lvname(lv));
		if (!(seg = _convert_lv_to_raid1(lv, "_rimage_0")))
			return 0;
	}

	/* raid0_meta: need to add an rmeta device to pair it with the given linear device as well */
	if (segtype_is_raid0_meta(new_segtype)) {
		log_debug_metadata("Adding raid metadata device to %s",
				   display_lvname(lv));
		if (!_alloc_and_add_rmeta_devs_for_lv(lv))
			return 0;
	}

	/* HM FIXME: overloading force argument here! */
	/* We may be called to convert to !raid0*, i.e. an interim conversion on the way to radi4/5/6 */
	if (force)
		return 1;

	seg->segtype = new_segtype;
	seg->region_size = 0;

	log_debug_metadata("Updating metadata and reloading mappings for %s",
			   display_lvname(lv));

	return lv_update_and_reload_origin(lv);
}

/* Helper: linear/raid0 with 1 image <-> raid1/4/5 takeover handler for @lv */
TAKEOVER_HELPER_FN(_linear_raid14510)
{
	struct lv_segment *seg = first_seg(lv);
	struct dm_list data_lvs, meta_lvs;
	struct segment_type *segtype;

	dm_list_init(&data_lvs);
	dm_list_init(&meta_lvs);

	if ((segtype_is_raid4(new_segtype) || segtype_is_any_raid5(new_segtype)) &&
	    new_image_count != 2) {
		log_error("Can't convert %s from %s to %s != 2 images",
			  display_lvname(lv), SEG_TYPE_NAME_LINEAR, new_segtype->name);
		return 0;
	}
#if 0
	/* HM FIXME: elaborate this raid4 restriction more! */
	if (segtype_is_raid4(new_segtype)) {
		log_error("Can't convert %s from %s to %s, please use %s",
			  display_lvname(lv), SEG_TYPE_NAME_LINEAR,
			  SEG_TYPE_NAME_RAID4, SEG_TYPE_NAME_RAID5);
		return 0;
	}
#endif
	if (!_yes_no_conversion(lv, new_segtype, yes, force, new_image_count, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	new_image_count = new_image_count > 1 ? new_image_count : 2;

	/* HM FIXME: overloading force argument to avoid metadata update in _linear_raid0() */
	/* Use helper _linear_raid0() to create the initial raid0_meta with one image pair up */
	if (!(segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID0_META)) ||
	    !_linear_raid0(lv, segtype, 0, 1 /* force */, 1, 1 /* data_copies */, 0, 0, allocate_pvs))
		return 0;

	/* Allocate the additional meta and data lvs requested */
	log_debug_metadata("Allocating %u additional data and metadata image pairs for %s",
			   new_image_count - 1, display_lvname(lv));
	if (!_lv_change_image_count(lv, new_segtype, new_image_count, allocate_pvs, NULL))
		return 0;

	seg = first_seg(lv);
	seg->segtype = new_segtype;

	return _lv_update_and_reload_origin_eliminate_lvs(lv, NULL);
}

/* Helper: striped/raid0* -> raid4/5/6/10 */
TAKEOVER_HELPER_FN(_striped_raid0_raid45610)
{
	struct lv_segment *seg = first_seg(lv);

	if (!_yes_no_conversion(lv, new_segtype, yes, force, new_image_count, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	/* This helper can be used to convert from raid0* -> raid10 too */
	if (seg_is_striped(seg)) {
		log_debug_metadata("Coverting LV %s from %s to %s",
				   display_lvname(lv), SEG_TYPE_NAME_STRIPED, SEG_TYPE_NAME_RAID0);
		if (!(seg = _convert_striped_to_raid0(lv, 0 /* alloc_metadata_devs */, 0 /* update_and_reload */)))
			return 0;
	}

	/* Add metadata LVs */
	if (seg_is_raid0(seg)) {
		log_debug_metadata("Adding metadata LVs to %s", display_lvname(lv));
		if (!_raid0_add_or_remove_metadata_lvs(lv, 0 /* !update_and_reload */, NULL))
			return 0;
	}

	/* Add the additional component LV pairs */
	log_debug_metadata("Adding component LV pairs to %s", display_lvname(lv));
	if (!_lv_change_image_count(lv, new_segtype, new_image_count, allocate_pvs, NULL))
		return 0;

	/* If this is any raid5 conversion request -> enforce raid5_n, because we convert from striped */
	if (segtype_is_any_raid5(new_segtype)) {
		if (!segtype_is_raid5_n(new_segtype)) {
			log_warn("Overwriting requested raid type %s with %s to allow for conversion",
				 new_segtype->name, SEG_TYPE_NAME_RAID5_N);
	    		if (!(new_segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID5_N)))
				return 0;
		}

	/* If this is any raid6 conversion request -> enforce raid6_n_6, because we convert from striped */
	} else if (segtype_is_any_raid6(new_segtype)) {
		if (!segtype_is_raid6_n_6(new_segtype)) {
			log_warn("Overwriting requested raid type %s with %s to allow for conversion",
				 new_segtype->name, SEG_TYPE_NAME_RAID6_N_6);
			if (!(new_segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID6_N_6)))
				return 0;
		}

	/* If this is a raid10 conversion request -> reorder the areas to suit raid19 */
	} else if (segtype_is_raid10_near(new_segtype)) {
		log_debug_metadata("Reordering areas for raid0 -> raid10 takeover");
		if (!_raid10_reorder_seg_areas(seg, reorder_to_raid10))
			return 0;

	} else {
		log_error("Can't convert %s", display_lvname(lv));
		return 0;
	}


	seg->segtype = new_segtype;

	return _lv_update_and_reload_origin_eliminate_lvs(lv, NULL);
}

/* raid0 -> linear */
TAKEOVER_HELPER_FN(_raid0_linear)
{
	struct dm_list removal_lvs;

	dm_list_init(&removal_lvs);

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	log_debug_metadata("Converting %s from %s to %s",
			   display_lvname(lv),
			   SEG_TYPE_NAME_RAID0, SEG_TYPE_NAME_LINEAR);
	if (!_convert_raid_to_linear(lv, &removal_lvs))
		return_0;

	if (!(first_seg(lv)->segtype = get_segtype_from_string(lv->vg->cmd, SEG_TYPE_NAME_STRIPED)))
		return_0;

	/* HM FIXME: overloading force argument here! */
	if (force)
		return 1;

	return _lv_update_and_reload_origin_eliminate_lvs(lv, &removal_lvs);
}

/* Helper: raid0* with one image -> mirror */
TAKEOVER_HELPER_FN(_raid0_mirror)
{
	struct lv_segment *seg = first_seg(lv);
	struct segment_type *segtype;

	if (seg->area_count != 1)
		return _error(lv, new_segtype, yes, force, 0, 0 /* data_copies */, 0, 0, NULL);

	new_image_count = new_image_count > 1 ? new_image_count : 2;

	if (!_check_max_mirror_devices(new_image_count))
		return 0;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, new_image_count, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	if (seg_is_raid0(first_seg(lv))) {
		log_debug_metadata("Adding raid metadata device to %s",
				   display_lvname(lv));
		if (!_alloc_and_add_rmeta_devs_for_lv(lv))
			return 0;
	}

	/* First convert to raid1... */
	if (!(segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID1)))
		return 0;

	log_debug_metadata("Converting %s from %s to %s adding %u image component pairs",
			   display_lvname(lv),
			   seg->segtype->name, new_segtype->name,
			   new_image_count - seg->area_count);
	if (!_linear_raid14510(lv, segtype, 0, 0, new_image_count, 1 /* data_copies */, 0 /* new_stripes */,
			       new_stripe_size, allocate_pvs))
		return 0;

	/* ...second convert to mirror */
	log_debug_metadata("Converting %s from %s to %s",
			   display_lvname(lv),
			   segtype->name, new_segtype->name);
	return _convert_raid1_to_mirror(lv, new_segtype, new_image_count, allocate_pvs,
					1 /* !update_and_reload */, NULL);
}

/* raid0 with one image -> raid1 */
TAKEOVER_HELPER_FN(_raid0_raid1)
{
	struct lv_segment *seg = first_seg(lv);

	if (seg->area_count != 1)
		return _error(lv, new_segtype, yes, force, 0, 0 /* data_copies */, 0, 0, NULL);

	new_image_count = new_image_count > 1 ? new_image_count : 2;

	if (!_check_max_raid_devices(new_image_count))
		return 0;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, new_image_count, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	if (seg_is_raid0(seg)) {
		log_debug_metadata("Adding raid metadata device to %s",
				   display_lvname(lv));
		if (!_alloc_and_add_rmeta_devs_for_lv(lv))
			return 0;
	}

	log_debug_metadata("Converting %s from %s to %s adding %u image component pairs",
			   display_lvname(lv),
			   seg->segtype->name, new_segtype->name,
			   new_image_count - seg->area_count);
	seg->segtype = new_segtype;
	if (!_lv_change_image_count(lv, new_segtype, new_image_count, allocate_pvs, NULL))
		return 0;

	/* Master leg is the first sub lv */
	seg_lv(seg, 0)->status &= ~LV_REBUILD;

	return _lv_update_and_reload_origin_eliminate_lvs(lv, NULL);
}

/* Helper: mirror -> raid0* */
TAKEOVER_HELPER_FN(_mirror_raid0)
{
	struct lv_segment *seg = first_seg(lv);
	struct dm_list removal_lvs;

	dm_list_init(&removal_lvs);

	if (!seg_is_mirrored(seg)) {
		log_error(INTERNAL_ERROR "Can't convert non-mirrored segment of lv %s",
			  display_lvname(lv));
		return 0;
	}

	if (!_lv_is_synced(lv))
		return 0;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, new_image_count, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	log_debug_metadata("Converting mirror lv %s to raid", display_lvname(lv));
	if (!_convert_mirror_to_raid(lv, new_segtype, 1, allocate_pvs,
				     0 /* update_and_reaload */, &removal_lvs))
		return 0;

	if (segtype_is_raid0(new_segtype)) {
		/* Remove rmeta LVs */
		log_debug_metadata("Extracting and renaming metadata LVs from lv %s",
				   display_lvname(lv));
		if (!_extract_image_component_list(seg, RAID_META, 0, &removal_lvs))
			return 0;
	}

	seg->segtype = new_segtype;

	return _lv_update_and_reload_origin_eliminate_lvs(lv, &removal_lvs);
}

/* Helper: convert mirror with 2 images <-> raid4/5 */
TAKEOVER_HELPER_FN(_mirror_r45)
{
	struct lv_segment *seg = first_seg(lv);
	struct dm_list removal_lvs;

	dm_list_init(&removal_lvs);

	if (seg->area_count != 2) {
		log_error("Can't convert %s between %s and %s/%s with != 2 images",
			  display_lvname(lv), SEG_TYPE_NAME_MIRROR,
			  SEG_TYPE_NAME_RAID4, SEG_TYPE_NAME_RAID5);
		return 0;
	}

	if (!_lv_is_synced(lv))
		return 0;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, new_image_count, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	if (segtype_is_mirror(new_segtype)) {
		if (!_lv_free_reshape_space(lv)) {
			log_error(INTERNAL_ERROR "Failed to free reshape space");
			return 0;
		}

		if (!(seg->segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID1)) ||
		    !_convert_raid1_to_mirror(lv, new_segtype, 2, allocate_pvs,
					      0 /* !update_and_reload */, &removal_lvs))
			return 0;

	} else if (!_convert_mirror_to_raid(lv, new_segtype, 0, NULL, 0 /* update_and_reaload */, NULL))
		return 0;

	return _lv_update_and_reload_origin_eliminate_lvs(lv, &removal_lvs);
}

/* Helper: raid1 -> raid0* */
TAKEOVER_HELPER_FN(_raid1_raid0)
{
	struct lv_segment *seg = first_seg(lv);
	struct dm_list removal_lvs;

	dm_list_init(&removal_lvs);

	if (!seg_is_raid1(seg)) {
		log_error(INTERNAL_ERROR "Can't convert non-raid1 lv %s",
			  display_lvname(lv));
		return 0;
	}

	if (!_raid_in_sync(lv))
		return 0;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, new_image_count, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	seg->segtype = new_segtype;
	if (!_lv_change_image_count(lv, new_segtype, 1, allocate_pvs, &removal_lvs))
		return 0;

	/* Remove rmeta last LV if raid0  */
	if (segtype_is_raid0(new_segtype)) {
		log_debug_metadata("Extracting and renaming metadata LVs frim lv %s",
				   display_lvname(lv));
		if (!_extract_image_component_list(seg, RAID_META, 0, &removal_lvs))
			return 0;
	}

	return _lv_update_and_reload_origin_eliminate_lvs(lv, &removal_lvs);
}

/* raid45 -> raid0* / striped */
TAKEOVER_HELPER_FN(_r456_r0_striped)
{
	struct lv_segment *seg = first_seg(lv);
	struct dm_list removal_lvs;

PFLA("new_stripes=%u new_image_count=%u", new_stripes, new_image_count);
	if (!seg_is_raid4(seg) && !seg_is_raid5_n(seg) && !seg_is_raid6_n_6(seg)) {
		log_error("LV %s has to be of type raid4/raid5_n/raid6_n_6 to allow for this conversion",
			  display_lvname(lv));
		return 0;
	}

	if (!_raid_in_sync(lv))
		return 0;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, new_image_count, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	dm_list_init(&removal_lvs);

	if (!_lv_free_reshape_space(lv)) {
		log_error(INTERNAL_ERROR "Failed to free reshape space");
		return 0;
	}

	/* Remove meta and data lvs requested */
	if (!_lv_change_image_count(lv, new_segtype, new_image_count, allocate_pvs, &removal_lvs))
		return 0;

	if (!(seg->segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID0_META)))
		return_0;
seg = first_seg(lv);
PFLA("seg->area_count=%u", seg->area_count);
	if (segtype_is_striped(new_segtype)) {
		if (!_convert_raid0_to_striped(lv, 0, &removal_lvs))
			return_0;

	} else if (segtype_is_raid0(new_segtype) &&
		   !_raid0_add_or_remove_metadata_lvs(lv, 0 /* update_and_reload */, &removal_lvs))
		return_0;
	else {
		log_error(INTERNAL_ERROR "Called with wrong new segment type %s", new_segtype->name);
		return 0;
	}

	return _lv_update_and_reload_origin_eliminate_lvs(lv, &removal_lvs);
}

/* helper raid1 with N images or raid4/5* with 2 images <-> linear */
TAKEOVER_HELPER_FN(_raid14510_linear)
{
	struct dm_list removal_lvs;
	struct lv_segment *seg = first_seg(lv);

	dm_list_init(&removal_lvs);

PFL();
	/* Only raid1 may have != 2 images when converting to linear */
	if (seg->area_count > 2 && !seg_is_raid1(seg)) {
		log_error("Can't convert %s with!%u images",
			  display_lvname(lv), seg->area_count);
		return 0;
	}

PFL();
	if (!_raid_in_sync(lv))
		return 0;

PFL();
	if (!_yes_no_conversion(lv, new_segtype, yes, force, new_image_count, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;
PFL();
	/*
	 * Have to remove any reshape space which my be a the beginning of
	 * the component data images or linear ain't happy about data content
	 */
	if (!_lv_free_reshape_space(lv)) {
		log_error(INTERNAL_ERROR "Failed to free reshape space");
		return 0;
	}

	/* Reduce image count to one */
	if (!(seg->segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID1)) ||
	    !_lv_change_image_count(lv, new_segtype, 1, allocate_pvs, &removal_lvs))
		return 0;

	if (!_convert_raid_to_linear(lv, &removal_lvs))
		return_0;

	return _lv_update_and_reload_origin_eliminate_lvs(lv, &removal_lvs);
}

/* Helper: raid1 with N images to N images (N != M) and raid4/5 to raid6* */
TAKEOVER_HELPER_FN(_raid145_raid1_raid6) 
{
	struct dm_list removal_lvs;

	dm_list_init(&removal_lvs);

	if (!_raid_in_sync(lv))
		return 0;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, new_image_count, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	if (!_lv_change_image_count(lv, new_segtype, new_image_count, allocate_pvs, &removal_lvs))
		return 0;

	first_seg(lv)->segtype = new_segtype;
	if (segtype_is_raid1(new_segtype))
		first_seg(lv)->data_copies = new_image_count;

	return _lv_update_and_reload_origin_eliminate_lvs(lv, &removal_lvs);
}

/* Helper: raid1/5 with 2 images <-> raid4/5/10 or raid4 <-> raid5_n with any image count */
TAKEOVER_HELPER_FN(_raid145_raid4510)
{
	struct lv_segment *seg = first_seg(lv);

	if (!_raid_in_sync(lv))
		return 0;

	if (new_image_count)
		log_error("Ignoring new image count for %s", display_lvname(lv));
	
	new_image_count = seg->area_count;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, new_image_count, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	/*
	 * In case I convert to a non-reshapable mapping, I have to remove
	 * any reshape space which may be at the beginning of the component
	 * data images or the data content will be mapped to an offset
	 */
	if (!segtype_is_reshapable_raid(new_segtype) &&
	    !_lv_free_reshape_space(lv)) {
		log_error(INTERNAL_ERROR "Failed to free reshape space");
		return 0;
	}

	if (seg_is_raid1(seg))
		seg->data_copies = 1;

	if (seg_is_raid4(seg) && segtype_is_any_raid5(new_segtype)) {
		if (!(seg->segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID5_N)))
			return_0;
	} else
		seg->segtype = new_segtype;

	return _lv_update_and_reload_origin_eliminate_lvs(lv, NULL);
}

/* Helper: raid10 -> striped/raid0* */
/* HM FIXME: enhance to support raid10_far -> stripes/raid0* */
TAKEOVER_HELPER_FN_REMOVAL_LVS(_raid10_striped_r0)
{
	struct lv_segment *seg = first_seg(lv);

	if (!seg_is_raid10_near(seg)) {
		log_error("Can't convert %s LV %s to %s",
			  seg->segtype->name, display_lvname(lv), new_segtype->name);
		log_error("You may want to convert to raid10_near with even number of stripes first");
		return 0;
	}

	if (seg->area_count % 2) {
		log_error("Can't convert %s LV %s with odd number of stripes to %s",
			  seg->segtype->name, display_lvname(lv), new_segtype->name);
		return 0;
	}

	if (!_raid_in_sync(lv))
		return 0;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, new_image_count, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	log_debug_metadata("Reordering areas for raid10 -> raid0 takeover");
	if (!_raid10_reorder_seg_areas(seg, reorder_from_raid10))
		return 0;

	if (!seg->data_copies)
		seg->data_copies = 2;

	new_image_count = seg->area_count / seg->data_copies;

	/* Remove the last half of the meta and data image pairs */
	log_debug_metadata("Removing data and metadata image LV pairs from %s", display_lvname(lv));
	if (!_lv_change_image_count(lv, new_segtype, new_image_count, allocate_pvs, removal_lvs))
		return 0;

	if (!segtype_is_any_raid0(new_segtype)) {
		if (!(seg->segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID0_META)))
			return_0;

		if (!_convert_raid0_to_striped(lv, 0, removal_lvs))
			return 0;

		seg = first_seg(lv);
	} else if (segtype_is_raid0(new_segtype) &&
		   !_raid0_add_or_remove_metadata_lvs(lv, 0 /* update_and_reload */, removal_lvs))
		return 0;

PFLA("seg->stripe_size=%u", seg->stripe_size);
PFLA("seg->chunk_size=%u", seg->chunk_size);
	seg->segtype = new_segtype;

	/* HM FIXME: overloading force argument here! */
	return force ? 1 : _lv_update_and_reload_origin_eliminate_lvs(lv, removal_lvs);
}

/* Helper: raid10 with 2 images <-> raid1/raid4/raid5* */
TAKEOVER_HELPER_FN(_raid10_r1456)
{
	struct lv_segment *seg = first_seg(lv);
	struct dm_list removal_lvs;

	dm_list_init(&removal_lvs);

	if (seg->area_count != 2 && seg_is_raid1(seg)) {
		log_error("Can't convert %s from %s to %s with != 2 images",
			  display_lvname(lv), lvseg_name(seg), new_segtype->name);
		return 0;
	}

	if (!_raid_in_sync(lv))
		return 0;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, new_image_count, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	first_seg(lv)->segtype = new_segtype;

	return _lv_update_and_reload_origin_eliminate_lvs(lv, &removal_lvs);
}

/* End takeover helper funtions */

/*
 * Begin all takeover functions referenced via the 2-dimensional _takeover_fn[][] matrix
 */
/* Linear -> raid0 */
TAKEOVER_FN(_l_r0)
{
	return _lv_has_segments_with_n_areas(lv, 1) &&
	       _linear_raid0(lv, new_segtype, yes, force, 1, 1, 0, 0, allocate_pvs);
}

/* Linear -> raid1 */
TAKEOVER_FN(_l_r1)
{
	return _lv_has_segments_with_n_areas(lv, 1) &&
	       _linear_raid14510(lv, new_segtype, yes, force,
				 new_image_count, 1, 0 /* new_stripes */,
				 new_stripe_size, allocate_pvs);
}

/* Linear -> raid4/5 */
TAKEOVER_FN(_l_r45)
{
	if (!_lv_has_segments_with_n_areas(lv, 1))
		return 0;

	return _linear_raid14510(lv, new_segtype, yes, force,
				 2 /* new_image_count */, 1, 0 /* new_stripes */,
				 new_stripe_size, allocate_pvs);
}

/* Linear -> raid10 */
TAKEOVER_FN(_l_r10)
{
	return _lv_has_segments_with_n_areas(lv, 1) &&
	       _linear_raid14510(lv, new_segtype, yes, force,
				 2 /* new_image_count */ , 1, 0 /* new_stripes */,
				 new_stripe_size, allocate_pvs);
}

/* Striped -> raid0 */
TAKEOVER_FN(_s_r0)
{
	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	return _convert_striped_to_raid0(lv, 0 /* !alloc_metadata_devs */, 1 /* update_and_reload */) ? 1 : 0;
}

/* Striped -> raid0_meta */
TAKEOVER_FN(_s_r0m)
{
	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	return _convert_striped_to_raid0(lv, 1 /* alloc_metadata_devs */, 1 /* update_and_reload */) ? 1 : 0;
} 

/* Striped -> raid4/5 */
TAKEOVER_FN(_s_r45)
{
	return _striped_raid0_raid45610(lv, new_segtype, yes, force, first_seg(lv)->area_count + 1, 0 /* data_copies*/, 0, 0, allocate_pvs);
}

/* Striped -> raid6 */
TAKEOVER_FN(_s_r6)
{
	return _striped_raid0_raid45610(lv, new_segtype, yes, force, first_seg(lv)->area_count + 2, 0, 0 /* data_copies*/, 0, allocate_pvs);
}

TAKEOVER_FN(_s_r10)
{
	return _striped_raid0_raid45610(lv, new_segtype, yes, force, first_seg(lv)->area_count * 2, 0 /* data_copies*/, 0, 0, allocate_pvs);
}

/* mirror -> raid0 */
TAKEOVER_FN(_m_r0)
{
	return _mirror_raid0(lv, new_segtype, yes, force, 1, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* mirror -> raid0_meta */
TAKEOVER_FN(_m_r0m)
{
	return _mirror_raid0(lv, new_segtype, yes, force, 1, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* Mirror -> raid1 */
TAKEOVER_FN(_m_r1)
{
	struct dm_list removal_lvs;

	dm_list_init(&removal_lvs);

	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	return _convert_mirror_to_raid(lv, new_segtype, new_image_count, allocate_pvs,
				       1 /* update_and_reaload */, &removal_lvs);
}

/* Mirror with 2 images -> raid4/5 */
TAKEOVER_FN(_m_r45)
{
	return _mirror_r45(lv, new_segtype, yes, force, 0, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* Mirror with 2 images -> raid10 */
TAKEOVER_FN(_m_r10)
{
	struct lv_segment *seg = first_seg(lv);

	if (seg->area_count != 2) {
		log_error("Can't convert %s from %s to %s with != 2 images",
			  display_lvname(lv), SEG_TYPE_NAME_MIRROR, new_segtype->name);
		return 0;
	}

	if (!_lv_is_synced(lv))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	if (!_convert_mirror_to_raid(lv, new_segtype, 0, NULL, 0 /* update_and_reaload */, NULL))
		return 0;

	seg->segtype = new_segtype;

	return lv_update_and_reload(lv);;
}


/* raid0 -> linear */
TAKEOVER_FN(_r0_l)
{
	return _raid0_linear(lv, new_segtype, yes, force, 0, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* raid0 with one image -> mirror */
TAKEOVER_FN(_r0_m)
{
	return _raid0_mirror(lv, new_segtype, yes, force, new_image_count, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* raid0 -> raid0_meta */
TAKEOVER_FN(_r0_r0m)
{
	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	return _raid0_add_or_remove_metadata_lvs(lv, 1, NULL);
}

/* raid0 -> striped */
TAKEOVER_FN(_r0_s)
{
	struct dm_list removal_lvs;

	dm_list_init(&removal_lvs);

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	return _convert_raid0_to_striped(lv, 1, &removal_lvs);
}

/* raid0 with one image -> raid1 */
TAKEOVER_FN(_r0_r1)
{
	return _raid0_raid1(lv, new_segtype, yes, force, new_image_count, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* raid0 -> raid4/5_n */
TAKEOVER_FN(_r0_r45)
{
	return _striped_raid0_raid45610(lv, new_segtype, yes, force, first_seg(lv)->area_count + 1, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* raid0 -> raid6_n_6 */
TAKEOVER_FN(_r0_r6)
{
	return _striped_raid0_raid45610(lv, new_segtype, yes, force, first_seg(lv)->area_count + 2, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* raid0 with N images (N > 1) -> raid10 */
TAKEOVER_FN(_r0_r10)
{
	return _striped_raid0_raid45610(lv, new_segtype, yes, force, first_seg(lv)->area_count * 2, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* raid0_meta -> */
TAKEOVER_FN(_r0m_l)
{
	return _raid0_linear(lv, new_segtype, yes, force, 0, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* raid0_meta -> mirror */
TAKEOVER_FN(_r0m_m)
{
	return _raid0_mirror(lv, new_segtype, yes, force, new_image_count, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* raid0_meta -> raid0 */
TAKEOVER_FN(_r0m_r0)
{
	struct dm_list removal_lvs;

	dm_list_init(&removal_lvs);

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	return _raid0_add_or_remove_metadata_lvs(lv, 1, &removal_lvs);
}

/* raid0_meta -> striped */
TAKEOVER_FN(_r0m_s)
{
	struct dm_list removal_lvs;

	dm_list_init(&removal_lvs);

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	return _convert_raid0_to_striped(lv, 1, &removal_lvs);
}

/* raid0_meta wih 1 image -> raid1 */
TAKEOVER_FN(_r0m_r1)
{
	return _raid0_raid1(lv, new_segtype, yes, force, new_image_count, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* raid0_meta -> raid4/5_n */
TAKEOVER_FN(_r0m_r45)
{
	return _striped_raid0_raid45610(lv, new_segtype, yes, force, first_seg(lv)->area_count + 1, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* raid0_meta -> raid6_n_6 */
TAKEOVER_FN(_r0m_r6)
{
	return _striped_raid0_raid45610(lv, new_segtype, yes, force, first_seg(lv)->area_count + 2, 0 /* data_copies */, 0, 0, allocate_pvs);
}


/* raid0_meta wih 1 image -> raid10 */
TAKEOVER_FN(_r0m_r10)
{
	return _striped_raid0_raid45610(lv, new_segtype, yes, force, first_seg(lv)->area_count * 2, 0 /* data_copies */, 0, 0, allocate_pvs);
}


/* raid1 with N images -> linear */
TAKEOVER_FN(_r1_l)
{
PFL();
	return _raid14510_linear(lv, NULL, yes, force, 1, 1, 0, 0, allocate_pvs);
}

/* raid1 -> mirror */
TAKEOVER_FN(_r1_m)
{
	struct dm_list removal_lvs;

	dm_list_init(&removal_lvs);

	if (!_raid_in_sync(lv))
		return 0;

	/*
	 * FIXME: support this conversion or don't invite users to switch back to "mirror"?
	 *        I find this at least valuable in case of an erroneous conversion to raid1
	 */
	if (!yes && yes_no_prompt("WARNING: Do you really want to convert %s to "
				  "non-recommended \"%s\" type? [y/n]: ",
			  display_lvname(lv), SEG_TYPE_NAME_MIRROR) == 'n') {
		log_warn("Logical volume %s NOT converted to \"%s\"",
			  display_lvname(lv), SEG_TYPE_NAME_MIRROR);
		return 0;
	}
	if (sigint_caught())
		return_0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	return _convert_raid1_to_mirror(lv, new_segtype, new_image_count, allocate_pvs, 1, &removal_lvs);
}


/* raid1 -> raid0 */
TAKEOVER_FN(_r1_r0)
{
	return _raid1_raid0(lv, new_segtype, yes, force, 1, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* raid1 -> raid0_meta */
TAKEOVER_FN(_r1_r0m)
{
	return _raid1_raid0(lv, new_segtype, yes, force, 1, 0 /* data_copies */, 0, 0, allocate_pvs);
}

TAKEOVER_FN(_r1_r1) 
{

	return _raid145_raid1_raid6(lv, new_segtype, yes, force, new_image_count, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* raid1 with 2 legs -> raid10 */
TAKEOVER_FN(_r1_r45)
{
	if (first_seg(lv)->area_count != 2) {
		log_error("Can't convert %s from %s to %s with != 2 images",
			  display_lvname(lv),
			  SEG_TYPE_NAME_RAID1, new_segtype->name);
		return 0;
	}

	return _raid145_raid4510(lv, new_segtype, yes, force, new_image_count, 0 /* data_copies */, 0, 0, allocate_pvs);
}
/****************************************************************************/

/* raid1 with 2 legs or duplicating one -> raid10 */
TAKEOVER_FN(_r1_r10)
{
	if (first_seg(lv)->area_count != 2) {
		log_error("Can't convert %s from %s to %s with != 2 images",
			  display_lvname(lv),
			  SEG_TYPE_NAME_RAID1, new_segtype->name);
		return 0;
	}

	return _raid145_raid4510(lv, new_segtype, yes, force, new_image_count, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* raid45 with 2 images -> linear */
TAKEOVER_FN(_r45_l)
{
	if (first_seg(lv)->area_count != 2) {
		log_error("Can't convert %s from %s/%s to %s with != 2 images",
			  display_lvname(lv), SEG_TYPE_NAME_RAID4,
			  SEG_TYPE_NAME_RAID5, SEG_TYPE_NAME_LINEAR);
		return 0;
	}

	return _raid14510_linear(lv, NULL, yes, force, 1, 1, 0, 0, allocate_pvs);
}

/* raid4/5 -> striped */
TAKEOVER_FN(_r45_s)
{
PFL();
	return _r456_r0_striped(lv, new_segtype, yes, force, first_seg(lv)->area_count - 1, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* raid4/5 with 2 images -> mirror */
TAKEOVER_FN(_r45_m)
{
	return _mirror_r45(lv, new_segtype, yes, force, 0, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* raid4/5 -> raid0 */
TAKEOVER_FN(_r45_r0)
{
	return _r456_r0_striped(lv, new_segtype, yes, force, first_seg(lv)->area_count - 1, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* raid4/5 -> raid0_meta */
TAKEOVER_FN(_r45_r0m)
{
	return _r456_r0_striped(lv, new_segtype, yes, force, first_seg(lv)->area_count - 1, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* raid4 with 2 images or raid5_n with 3 images -> raid1 */
TAKEOVER_FN(_r45_r1)
{
	struct lv_segment *seg = first_seg(lv);

	if ((seg_is_raid5_n(seg) && seg->area_count != 3) ||
	    seg->area_count != 2) {
		log_error("Can't convert %s from %s to %s with != %u images",
			  display_lvname(lv), lvseg_name(seg), SEG_TYPE_NAME_RAID1,
			  seg_is_raid5_n(seg) ? 3 : 2);
		return 0;
	}

	return _raid145_raid4510(lv, new_segtype, yes, force, 2, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* raid4/5* <-> raid4/5* */
TAKEOVER_FN(_r45_r45)
{
	struct lv_segment *seg = first_seg(lv);

	if (seg->segtype == new_segtype) {
		log_warn("%s already is of type %s", display_lvname(lv), lvseg_name(seg));
		return 0;
	}

	/* Conversion to raid4 only possible raid5 with 2 legs or from raid5_n with any number of legs */
	if (segtype_is_raid4(new_segtype) &&
	    !(seg_is_any_raid5(seg) && seg->area_count == 2) &&
	    !seg_is_raid5_n(seg)) {
		log_error("Can't convert %s between %s and %s",
			  display_lvname(lv), lvseg_name(seg), SEG_TYPE_NAME_RAID4);
		log_error("Convert %s to %s first",
			  display_lvname(lv), SEG_TYPE_NAME_RAID5_N);
		return 0;
	}

#if 0
	if (!_lv_alloc_reshape_space(lv, alloc_anywhere, allocate_pvs))
		return 0;
#endif
	return _raid145_raid4510(lv, new_segtype, yes, force, first_seg(lv)->area_count, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* raid4/5* <-> raid6* */
TAKEOVER_FN(_r45_r6)
{
	struct lv_segment *seg = first_seg(lv);

	if (seg_is_raid4(seg)) {
		log_error("Please convert %s from %s to %s first",
			  display_lvname(lv), SEG_TYPE_NAME_RAID4,  SEG_TYPE_NAME_RAID5_N);
		return 0;
	}

	if (seg->area_count < 3) {
		log_error("Please convert %s from 1 stripe to at least 2 with \"lvconvert --stripes 2 %s\" "
			  "first for this conversion",
			  display_lvname(lv), display_lvname(lv));
		return 0;
	}

	if (segtype_is_any_raid6(new_segtype) &&
	    !(new_segtype = get_segtype_from_flag(lv->vg->cmd, _raid_seg_flag_5_to_6(seg)))) {
		log_error(INTERNAL_ERROR "Failed to get raid5 -> raid6 conversion type");
		return_0;
	}

	return _raid145_raid1_raid6(lv, new_segtype, yes, force, seg->area_count + 1, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* raid6 -> striped */
TAKEOVER_FN(_r6_s)
{
	return _r456_r0_striped(lv, new_segtype, yes, force, first_seg(lv)->area_count - 2, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* raid6 -> raid0 */
TAKEOVER_FN(_r6_r0)
{
	return _r456_r0_striped(lv, new_segtype, yes, force, first_seg(lv)->area_count - 2, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* raid6 -> raid0_meta */
TAKEOVER_FN(_r6_r0m)
{
	return _r456_r0_striped(lv, new_segtype, yes, force, first_seg(lv)->area_count - 2, 0 /* data_copies */, 0, 0, allocate_pvs);
}

/* raid6* -> raid4/5* */
TAKEOVER_FN(_r6_r45)
{
	struct lv_segment *seg = first_seg(lv);
	struct dm_list removal_lvs;

	if (!_raid_in_sync(lv))
		return 0;

	if (segtype_is_raid4(new_segtype) &&
	    !seg_is_raid6_n_6(seg)) {
		log_error("LV %s has to be of type %s to allow for this conversion",
			  display_lvname(lv), SEG_TYPE_NAME_RAID6_N_6);
		return 0;
	}

	if ((seg_is_raid6_zr(seg) ||
	     seg_is_raid6_nc(seg) ||
	     seg_is_raid6_nr(seg)) &&
	    !segtype_is_raid6_n_6(new_segtype)) {
		log_error("LV %s has to be of type %s,%s,%s,%s or %s to allow for direct conversion",
			  display_lvname(lv),
			  SEG_TYPE_NAME_RAID6_LS_6, SEG_TYPE_NAME_RAID6_LA_6,
			  SEG_TYPE_NAME_RAID6_RS_6, SEG_TYPE_NAME_RAID6_RA_6,
			  SEG_TYPE_NAME_RAID6_N_6);
		return 0;
	}

	new_image_count = seg->area_count - 1;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, new_image_count, 0, 0))
		return 0;

	dm_list_init(&removal_lvs);

	/* Remove meta and data lvs requested */
	log_debug_metadata("Removing one data and metadata image LV pair from %s", display_lvname(lv));
	if (!_lv_change_image_count(lv, new_segtype, new_image_count, allocate_pvs, &removal_lvs))
		return 0;

	if (!(seg->segtype = get_segtype_from_flag(lv->vg->cmd, _raid_seg_flag_6_to_5(seg)))) {
		log_error(INTERNAL_ERROR "Failed to get raid6 -> raid5 conversion type");
		return_0;
	}

	return _lv_update_and_reload_origin_eliminate_lvs(lv, &removal_lvs);
}


/* raid6* <-> raid6* */
TAKEOVER_FN(_r6_r6)
{
	struct lv_segment *seg = first_seg(lv);

	if (seg->segtype == new_segtype) {
		_noop(lv, NULL, 0, 0, 0, 0, 0, 0, allocate_pvs);
		return 0;
	}

	if (!_raid_in_sync(lv))
		return 0;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, seg->area_count, 0, 0))
		return 0;

	if (!_lv_alloc_reshape_space(lv, alloc_anywhere, allocate_pvs))
		return 0;

	seg->segtype = new_segtype;

	return _lv_update_and_reload_origin_eliminate_lvs(lv, NULL);
}

/* raid10 with 2 images -> linear */
TAKEOVER_FN(_r10_l)
{
	if (first_seg(lv)->area_count != 2) {
		log_error("Can't convert %s from %s to %s with != 2 images",
			  display_lvname(lv), SEG_TYPE_NAME_RAID10, SEG_TYPE_NAME_MIRROR);
		return 0;
	}

	return _raid14510_linear(lv, NULL, yes, force, 1, 1, 0, 0, allocate_pvs);
}

/* raid10 -> raid0* */
TAKEOVER_FN(_r10_s)
{
	struct dm_list removal_lvs;
PFL();
	dm_list_init(&removal_lvs);

#if 0
	if (first_seg(lv)->area_count != 2) {
		log_error("Can't convert %s from %s to %s with != 2 images",
			  display_lvname(lv), SEG_TYPE_NAME_RAID10, SEG_TYPE_NAME_MIRROR);
		return 0;
	}
#endif
	return _raid10_striped_r0(lv, new_segtype, yes, 0, 0, 0 /* data_copies */, 0, 0, allocate_pvs, &removal_lvs);
}

/* raid10 with 2 images -> mirror */
TAKEOVER_FN(_r10_m)
{
	struct lv_segment *seg = first_seg(lv);
	struct dm_list removal_lvs;

	dm_list_init(&removal_lvs);

	if (seg->area_count != 2) {
		log_error("Can't convert %s from %s to %s with != 2 images",
			  display_lvname(lv), SEG_TYPE_NAME_RAID10, SEG_TYPE_NAME_MIRROR);
		return 0;
	}

	if (!_raid_in_sync(lv))
		return 0;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, seg->area_count, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	/* HM FIXME: support -mN during this conversion */
	if (!(seg->segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID1)) ||
	    !_convert_raid1_to_mirror(lv, new_segtype, new_image_count, allocate_pvs, 0, &removal_lvs))
		return 0;

	return _lv_update_and_reload_origin_eliminate_lvs(lv, &removal_lvs);
}

/* raid10 -> raid0 */
TAKEOVER_FN(_r10_r0)
{
	struct dm_list removal_lvs;

	dm_list_init(&removal_lvs);

	return _raid10_striped_r0(lv, new_segtype, yes, 0, 0, 0 /* data_copies */, 0, 0, allocate_pvs, &removal_lvs);
}

/* raid10 -> raid0_meta */
TAKEOVER_FN(_r10_r0m)
{
	struct dm_list removal_lvs;

	dm_list_init(&removal_lvs);

	return _raid10_striped_r0(lv, new_segtype, yes, 0, 0, 0 /* data_copies */, 0, 0, allocate_pvs, &removal_lvs);
}

/* raid10 with 2 images -> raid1 */
TAKEOVER_FN(_r10_r1)
{
	return _lv_has_segments_with_n_areas(lv, 2) &&
	       _raid10_r1456(lv, new_segtype, yes, force, 2, 2, 0, 0, allocate_pvs);
}

/*
 * raid01 (data_copies on top of stripes
 */
static int _lv_create_raid01_image_lvs(struct logical_volume *lv,
				       const struct segment_type *segtype,
				       uint32_t len, uint32_t stripe_size,
				       uint32_t start, uint32_t end,
				       struct dm_list *allocate_pvs)
{
	uint32_t s, ss;
	char *image_name;
	struct logical_volume *image_lv;
	struct lv_segment *seg = first_seg(lv);

	if (end > seg->area_count)
		return 0;
	if (start > end)
		return 0;

	/* Create the N striped lvs to put under raid1 */
	log_debug_metadata("Creating %u stripes for %s", end - start, display_lvname(lv));
	for (s = start; s < end; s++) {
		if (!(image_name = _generate_raid_name(lv, "rimage", s)))
			return_0;
		/*
		 * Prevent any PVs holding image components of the just
		 * allocated raid0 striped LV from being used for allocation
		 */
		for (ss = 0; ss < s; ss++)
			if (!_avoid_pvs_with_other_images_of_lv(seg_lv(seg, ss), allocate_pvs)) {
				log_error("Failed to prevent PVs holding image components "
					  "from lv %s being used for allocation.",
					  display_lvname(seg_lv(seg, ss)));
				return 0;
			}

		if (!(image_lv = _lv_create(lv->vg, image_name, segtype, 1 /* data_copies */, end - start,
						  0 /* region_size */, stripe_size, len /* extents */, allocate_pvs))) {
			log_error("Failed to create striped image lv %s/%s", lv->vg->name, image_name);
			return 0;
		}
		lv_set_hidden(image_lv);
		image_lv->status |= RAID_IMAGE;

		log_debug_metadata("Setting %u stripe segment area for %s", s + 1, display_lvname(lv));
		if (!set_lv_segment_area_lv(seg, s, image_lv, 0 /* le */, seg->status | RAID_IMAGE))
			return_0;
	}

	return 1;
}

/* striped with any number of images to raid01 */
TAKEOVER_FN(_s_r01)
{
	struct lv_segment *seg, *striped_seg = first_seg(lv);

PFLA("new-data_copies=%u", new_data_copies);
	if (new_data_copies < 2)
		return 0;
PFL();
	log_debug_metadata("Converting lv %s to raid1", display_lvname(lv));
	if (!(seg = _convert_lv_to_raid1(lv, "_rimage_0")))
		return 0;
PFL();
	log_debug_metadata("Reallocating segment areas of %s", display_lvname(lv));
	if (!_realloc_seg_areas(lv, new_data_copies, RAID_IMAGE))
		return 0;

	seg->area_count = new_data_copies;
PFL();
	if (!_lv_create_raid01_image_lvs(lv, striped_seg->segtype, striped_seg->len,
					 striped_seg->stripe_size, 1, new_data_copies, allocate_pvs))
		return 0;

PFLA("seg->len=%u seg->area_len=%u", seg->len, seg->area_len);
	lv->le_count = seg->len;
	lv->size = seg->len * lv->vg->extent_size;
PFL();
	log_debug_metadata("Allocating %u metadata images for %s", new_data_copies, display_lvname(lv));
	seg->meta_areas = NULL; /* Reset to force rmeta device creation in raid01 segment */
	if (!_alloc_and_add_rmeta_devs_for_lv(lv))
		return 0;
PFL();
	return lv_update_and_reload(lv);
}

/* raid0 with any number of images to raid01 */
TAKEOVER_FN(_r0_r01)
{
	return 0;
}

/* raid0_meta with any number of images to raid01 */
TAKEOVER_FN(_r0m_r01)
{
	return 0;
}

/* raid01 with any number of data_copies to striped */
TAKEOVER_FN(_r01_s)
{
	uint32_t s;
	struct logical_volume *image_lv = NULL;
	struct lv_segment *seg = first_seg(lv);

	for (s = 0; s < seg->area_count; s++)
		if (_lv_is_synced(seg_lv(seg, s))) {
			image_lv = seg_lv(seg, s);
			break;
		}
PFL();
	if (!image_lv) {
		log_error("No mirror in sync!");
		return 0;
	}
PFL();
	for ( ; s < seg->area_count - 1; s++)
		seg_lv(seg, s) = seg_lv(seg, s + 1);
PFL();
	seg->area_count--;
	for (s = 0; s < seg->area_count; s++)
		if (!_replace_lv_with_error_segment(seg_lv(seg, s)))
			return_0;
PFL();
	if (!set_lv_segment_area_lv(seg, 0, image_lv, 0 /* le */, image_lv->status)) {
		log_error("Failed to add sublv %s", display_lvname(image_lv));
		return 0;
	}
PFL();
	if (!remove_layer_from_lv(lv, image_lv))
		return_0;
PFL();
	return lv_update_and_reload(lv);
}

/* raid01 with any number of data_copies to raid0 */
TAKEOVER_FN(_r01_r0)
{
	return 0;
}

/* raid01 with any number of data_copies to raid0_meta */
TAKEOVER_FN(_r01_r0m)
{
	return 0;
}

/* raid01 with any number of data_copies to raid45 */
TAKEOVER_FN(_r01_r45)
{
	return 0;
}

/* raid01 with any number of data_copies to raid10 */
TAKEOVER_FN(_r01_r10)
{
	return 0;
}

/* Change number of data_copies on raid01 */
TAKEOVER_FN(_r01_r01)
{
	return 0;
}

/*
 * 2-dimensional takeover function matrix defining the
 * FSM of possible/impossible or noop (i.e. requested
 * conversion already given on the lv) conversions
 *
 * Rows define segtype from and columns segtype to
 */
static takeover_fn_t _takeover_fn[][10] = {
	/* from, to ->     linear   striped  mirror   raid0    raid0_meta  raid1    raid4/5    raid6    raid10  raid01 */
	/*   | */
	/*   v */
	/* linear     */ { _noop,   _error,  _error,  _l_r0,   _l_r0,      _l_r1,   _l_r45,    _error,  _l_r10  , _error   },
	/* striped    */ { _error,  _noop,   _error,  _s_r0,   _s_r0m,     _l_r1,   _s_r45,    _s_r6,   _s_r10  , _s_r01   },
	/* mirror     */ { _error,  _error,  _noop,   _m_r0,   _m_r0m,     _m_r1,   _m_r45,    _error,  _m_r10  , _error   },
	/* raid0      */ { _r0_l,   _r0_s,   _r0_m,   _noop,   _r0_r0m,    _r0_r1,  _r0_r45,   _r0_r6,  _r0_r10 , _r0_r01  },
	/* raid0_meta */ { _r0m_l,  _r0m_s,  _r0m_m,  _r0m_r0, _noop,      _r0m_r1, _r0m_r45,  _r0m_r6, _r0m_r10, _r0m_r01 },
	/* raid1      */ { _r1_l,   _r1_l,   _r1_m,   _r1_r0,  _r1_r0m,    _r1_r1,  _r1_r45,   _error,  _r1_r10 , _error   },
	/* raid4/5    */ { _r45_l,  _r45_s,  _r45_m,  _r45_r0, _r45_r0m,   _r45_r1, _r45_r45,  _r45_r6, _error  , _error   },
	/* raid6      */ { _error,  _r6_s,   _error,  _r6_r0,  _r6_r0m,    _error,  _r6_r45,   _r6_r6,  _error  , _error   },
	/* raid10     */ { _r10_l,  _r10_s,  _r10_m,  _r10_r0, _r10_r0m,   _r10_r1, _error,    _error,  _error  , _error   },
	/* raid01     */ { _error,  _r01_s,  _error,  _r01_r0, _r01_r0m,   _error,  _r01_r45,  _error,  _r01_r10, _r01_r01 },
};

/* End: various conversions between layers (aka MD takeover) */
/****************************************************************************/

/*
 * lv_raid_convert
 * @lv
 * @new_segtype
 *
 * Convert @lv from one RAID type (or 'mirror' segtype) to @new_segtype,
 * change RAID algorithm (e.g. left symmetric to right asymmetric),
 * add/remove LVs to/from a RAID LV or change stripe sectors
 *
 * Non dm-raid changes are factored in e.g. "mirror" and "striped" related
 * fucntions called from here.
 * All the rest of the raid <-> raid conversions go into a function
 * _convert_raid_to_raid() of their own called from here.
 *
 * Returns: 1 on success, 0 on failure
 */
/*
 * [18:42] <lvmguy> agk: what has to be changed when getting "Performing unsafe table load while..."
 * [18:50] <agk> Ah, that depends on the context
 * [18:51] <agk> as you're doing something new, we need to look at the trace and work out what to do
 * [18:51] <agk> What it means is:
 * [18:52] <agk>    if a device is suspended, i/o might get blocked and you might be unable to allocate memory
 * [18:52] <agk>    doing a table load needs memory
 * [18:52] <agk> So if you have suspend + load, then you could get deadlock
 * [18:52] <agk> and it's warning about that 
 * [18:52] <agk> but not every situation is like that - there are false positives
 * [18:53] <agk> So get the -vvvv trace from the command, then grep out the ioctls
 * [18:53] <agk> and look at the sequence and see what is supended at the time of the load
 * [18:54] <agk> IOW a suspend can cause a later table load to block - and it won't clear until you get a resume - but that resume depends on the load completing, which isn't going to happen
 * [18:54] <lvmguy> I thought it was trying to prevent OOM. need analyze the details...
 * [18:54] <agk> so the code normally does:   load, suspend, resume   in that order
 * [18:54] <agk> never suspend, load, resume
 * [18:55] <agk> but when you get complex operations all that dependency tree code tries to deal with this
 * [18:56] <lvmguy> yep, the sequences I have to do look like they fall into this latter realm ;)
 * [18:56] <agk> - it tries to sort all the operations on the various devices into a safe order in which to perform them
 * [18:58] <agk> So normally, (1) get the actual list of operations it's performing.  (2) work out if there is an easy fix by performing them in a different order - if so, we work out how to change the code to do that (often needs hacks)
 * [18:59] <agk>   - if not, then we look for an alternative strategy, usually by splitting operations into more than one step which can be done within the dependency rules
 * [19:02] <lvmguy> let me figure out dependency details then we can discuss
 * [19:03] <agk> - kabi is *very* familiar with fixing these sorts of problems:)
 * [19:04] <agk>   - we had to go through it all for thin and cache
 * [19:04] <agk> But so far, we've not yet hit a situation we couldn't solve
 * [19:04] <lvmguy> k
 * */
/*
 * TODO:
 *  - review size calculations in raid1 <-> raid4/5
 *  - review stripe size usage on conversion from/to striped/nonstriped segment types
 *  - review reshape space alloc/free
 *  - conversion raid0 -> raid10 only mentions redundancy = 1 instead of 1..#stripes maximum
 *  - false --striped user entry shows wrong message
 *  - keep ti->len small on initial disk adding reshape and grow after it has finished
 *    in order to avoid bio_endio in the targets map method?
 */
int lv_raid_convert(struct logical_volume *lv,
		    const struct segment_type *new_segtype,
		    int yes, int force, int duplicate,
		    unsigned new_image_count,
		    const unsigned data_copies,
		    const unsigned new_stripes,
		    const unsigned new_stripe_size,
		    struct dm_list *allocate_pvs)
{
	uint32_t stripes = new_stripes, stripe_size;
	struct lv_segment *seg = first_seg(lv);
	struct segment_type *new_segtype_tmp = (struct segment_type *) new_segtype;
	struct segment_type *striped_segtype;
	struct dm_list removal_lvs;
	takeover_fn_t tfn;

	dm_list_init(&removal_lvs);

	if (!new_segtype) {
		log_error(INTERNAL_ERROR "New segtype not specified");
		return 0;
	}

	if (!(striped_segtype = get_segtype_from_string(lv->vg->cmd, SEG_TYPE_NAME_STRIPED)))
		return_0;

	/* Given segtype of @lv */
	if (!seg_is_striped(seg) && /* Catches linear = "overloaded striped with one area" as well */
	    !seg_is_mirror(seg) &&
	    !seg_is_raid(seg))
		goto err;

	/* Requested segtype */
	if (!segtype_is_linear(new_segtype) &&
	    !segtype_is_striped(new_segtype) &&
	    !segtype_is_mirror(new_segtype) &&
	    !segtype_is_raid(new_segtype))
		goto err;

	/* Define new image count if not passed in */
	new_image_count = new_image_count ?: seg->area_count;

PFLA("new_segtype=%s new_image_count=%u new_stripes=%u segtype=%s, seg->area_count=%u", new_segtype->name, new_image_count, new_stripes, seg->segtype->name, seg->area_count);

	if (!_check_max_raid_devices(new_image_count))
		return 0;

	/* Converting raid1 -> linear given "lvconvert -m0 ..." w/o "--type ..." */
	if (new_image_count == 1 &&
	    seg->segtype == new_segtype)
		new_segtype = striped_segtype;

	/* Converting linear to raid1 given "lvconvert -mN ..." (N > 0)  w/o "--type ..." */
	if (seg_is_linear(seg) &&
	    seg->segtype == new_segtype &&
	    new_image_count > 1 &&
	    !(new_segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID1)))
		return_0;
PFLA("new_segtype=%s new_image_count=%u segtype=%s, seg->area_count=%u", new_segtype->name, new_image_count, seg->segtype->name, seg->area_count);

	/* Define new stripe size if not passed in */
	stripe_size = new_stripe_size ?: seg->stripe_size;
	stripes = new_stripes ?: _data_rimages_count(seg, seg->area_count);
PFLA("new_segtype=%s new_image_count=%u new_stripes=%u stripes=%u", new_segtype->name, new_image_count, new_stripes, stripes);

	/* @lv has to be active to perform raid conversion operatons */
	if (!lv_is_active(lv)) {
		log_error("%s must be active to perform this operation.",
			  display_lvname(lv));
		return 0;
	}

	/* If clustered VG, @lv has to be active locally */
	/* HM FIXME: has to change whnever we'll support clustered raid1 */
	if (vg_is_clustered(lv->vg) && !lv_is_active_exclusive_locally(lv)) {
		log_error("%s must be active exclusive locally to"
			  " perform this operation.", display_lvname(lv));
		return 0;
	}

	/* Process any active duplicating conversion */
	if (_lv_is_duplicating(lv)) {
		if (!_raid_conv_duplicate(lv, new_segtype, yes, force, new_image_count, data_copies, stripes,
					  new_stripe_size, allocate_pvs)) {
			if (!_lv_is_duplicating(lv))
				_log_possible_conversion_types(lv);

			return 0;
		}

		goto out;
	}

	if (!duplicate) {
		/*
		 * Reshape of capable raid type requested
		 */
		switch (_reshape_requested(lv, new_segtype_tmp, new_stripes, new_stripe_size)) {
		case 0:
			break;
		case 1:
			if (!_raid_in_sync(lv)) {
				log_error("Unable to convert %s while it is not in-sync",
					  display_lvname(lv));
				return 0;
			}

			if (new_stripes && seg->segtype != new_segtype) {
				log_error("Can't reshape and takeover %s at the same time",
					  display_lvname(lv));
				return 0;
			}

			return _raid_reshape(lv, new_segtype, yes, force, stripes, stripe_size, allocate_pvs);
		case 2:
			/* Error if we got here with stripes and/or stripe size change requested */
			return 0;
		}
	}

	/*
	 * Table driven takeover, i.e. conversions from one segment type to another
	 */
	new_image_count = new_image_count > 1 ? new_image_count : stripes;
	if (stripes != _data_rimages_count(seg, seg->area_count))
		new_image_count = stripes + new_segtype->parity_devs;

	if (!segtype_is_raid(new_segtype))
		stripes = new_stripes ?: 1;

PFLA("new_segtype=%s new_image_count=%u new_stripes=%u stripes=%u", new_segtype->name, new_image_count, new_stripes, stripes);

	/* A conversion by duplication has been requested (i.e. create a new lv of the requested segtype etc.) */
	if (duplicate)
		return _raid_conv_duplicate(lv, new_segtype, yes, force, new_image_count, data_copies,
					    stripes, new_stripe_size, allocate_pvs);
		

	/*
	 * All non-duplicating conversion requests need to be 100% in-sync,
	 * because those'll be processed using md takeover features relying
	 * on in-sync crc/q-syndroms
	 */
	if (!_raid_in_sync(lv)) {
		log_error("Unable to convert %s while it is not in-sync",
			  display_lvname(lv));
		return 0;
	}

PFLA("new_segtype=%s new_image_count=%u new_stripes=%u stripes=%u", new_segtype->name, new_image_count, new_stripes, stripes);
	tfn = _takeover_fn[_takeover_fn_idx(seg->segtype, seg->area_count)][_takeover_fn_idx(new_segtype, new_image_count)];
	if (!tfn(lv, new_segtype, yes, force, new_image_count, data_copies, stripes, stripe_size, allocate_pvs)) {
		_log_possible_conversion_types(lv);
		return 0;
	}
out:
	log_print_unless_silent("Logical volume %s successfully converted.", display_lvname(lv));

	return 1;
err:
	/* FIXME: enhance message */
	log_error("Converting the segment type for %s (directly) from %s to %s"
		  " is not supported.", display_lvname(lv),
		  lvseg_name(seg), new_segtype_tmp->name);

	return 0;
}

/* Find space to replace partial @lv */
static int _remove_partial_multi_segment_image(struct logical_volume *lv,
					       struct dm_list *remove_pvs)
{
	uint32_t s, extents_needed;
	struct lv_segment *rm_seg, *raid_seg = first_seg(lv);
	struct logical_volume *rm_image = NULL;
	struct physical_volume *pv;

	if (!(lv->status & PARTIAL_LV)) {
		log_error(INTERNAL_ERROR "Called with non-partial LV %s.", display_lvname(lv));
		return 0;
	}

	for (s = 0; s < raid_seg->area_count; s++) {
		extents_needed = 0;
		rm_image = seg_lv(raid_seg, s);

		if ((rm_image->status & PARTIAL_LV) &&
		    lv_is_on_pvs(rm_image, remove_pvs) &&
		    dm_list_size(&rm_image->segments) > 1) {

			/* First, how many damaged extents are there */
			if (seg_metalv(raid_seg, s)->status & PARTIAL_LV)
				extents_needed += seg_metalv(raid_seg, s)->le_count;

			dm_list_iterate_items(rm_seg, &rm_image->segments)
				/*
				 * segment areas are for stripe, mirror, raid,
			 	 * etc.  We only need to check the first area
			 	 * if we are dealing with RAID image LVs.
			 	 */
				if (seg_type(rm_seg, 0) == AREA_PV &&
				    (seg_pv(rm_seg, 0)->status & MISSING_PV))
					extents_needed += rm_seg->len;

			log_debug("%u extents needed to repair %s",
				  extents_needed, rm_image->name);

			/* Second, do the other PVs have the space */
			dm_list_iterate_items(rm_seg, &rm_image->segments)
				pv = seg_pv(rm_seg, 0);

				if (seg_type(rm_seg, 0) == AREA_PV &&
		    		    // !lv_is_on_pvs(rm_image, remove_pvs) && /* <- HM added to avoid remove_pvs on allocation */
				    !(pv->status & MISSING_PV)) {
					if ((pv->pe_count - pv->pe_alloc_count) > extents_needed) {
						log_debug("%s has enough space for %s",
							  pv_dev_name(pv), rm_image->name);
						goto has_enough_space;
					}

					log_debug("Not enough space on %s for %s",
						  pv_dev_name(pv), rm_image->name);
			}
		}
	}

	/*
	 * This is likely to be the normal case - single
	 * segment images.
	 */
	return_0;

has_enough_space:
	/*
	 * Now we have a multi-segment, partial image that has enough
	 * space on just one of its PVs for the entire image to be
	 * replaced.  So, we replace the image's space with an error
	 * target so that the allocator can find that space (along with
	 * the remaining free space) in order to allocate the image
	 * anew.
	 */
	return _replace_lv_with_error_segment(rm_image);
}

/* HM Helper fn to generate LV names and set segment area lv */
static int _generate_name_and_set_segment(struct logical_volume *lv,
					  uint32_t s, uint32_t sd,
					  struct dm_list *lvs, char **tmp_names)
{
	struct lv_segment *raid_seg = first_seg(lv);
	struct lv_list *lvl = dm_list_item(dm_list_first(lvs), struct lv_list);

	dm_list_del(&lvl->list);

	if (!(tmp_names[sd] = _generate_raid_name(lv, s == sd ? "rmeta" : "rimage", s)))
		return_0;
	if (!set_lv_segment_area_lv(raid_seg, s, lvl->lv, 0, lvl->lv->status)) {
		log_error("Failed to add %s to %s", lvl->lv->name, lv->name);
		return 0;
	}

	lv_set_hidden(lvl->lv);
	return 1;
}

/*
 * lv_raid_replace
 * @lv
 * @remove_pvs
 * @allocate_pvs
 *
 * Replace the specified PVs.
 */
int lv_raid_replace(struct logical_volume *lv,
		    struct dm_list *remove_pvs,
		    struct dm_list *allocate_pvs)
{
	int partial_segment_removed = 0;
	uint32_t s, sd, match_count = 0;
	char **tmp_names;
	struct dm_list old_lvs;
	struct dm_list new_meta_lvs, new_data_lvs;
	struct lv_segment *raid_seg = first_seg(lv);
	struct lv_list *lvl;

	dm_list_init(&old_lvs);
	dm_list_init(&new_meta_lvs);
	dm_list_init(&new_data_lvs);

	/* Replacement for raid0 would cause data loss */
	if (seg_is_any_raid0(raid_seg)) {
		log_error("Replacement of devices in %s %s LV prohibited.",
			  display_lvname(lv), raid_seg->segtype->name);
		return 0;
	}

	if (lv->status & PARTIAL_LV)
		lv->vg->cmd->partial_activation = 1;

	if (!lv_is_active_exclusive_locally(lv_lock_holder(lv))) {
		log_error("%s must be active %sto perform this operation.",
			  display_lvname(lv),
			  vg_is_clustered(lv->vg) ? "exclusive locally " : "");
		return 0;
	}

	if (!_raid_in_sync(lv)) {
		log_error("Unable to replace devices in %s while it is"
			  " not in-sync.", display_lvname(lv));
		return 0;
	}

	if (!(tmp_names = dm_pool_zalloc(lv->vg->vgmem, 2 * raid_seg->area_count * sizeof(*tmp_names))))
		return_0;

	if (!archive(lv->vg))
		return_0;

	/*
	 * How many sub-LVs are being removed?
	 */
	for (s = 0; s < raid_seg->area_count; s++) {
		if ((seg_type(raid_seg, s) == AREA_UNASSIGNED) ||
		    (seg_metatype(raid_seg, s) == AREA_UNASSIGNED)) {
			log_error("Unable to replace RAID images while the "
				  "array has unassigned areas");
			return 0;
		}

		if (lv_is_virtual(seg_lv(raid_seg, s)) ||
		    lv_is_virtual(seg_metalv(raid_seg, s)) ||
		    lv_is_on_pvs(seg_lv(raid_seg, s), remove_pvs) ||
		    lv_is_on_pvs(seg_metalv(raid_seg, s), remove_pvs))
			match_count++;
	}

	if (!match_count) {
		log_verbose("%s does not contain devices specified"
			    " for replacement", display_lvname(lv));
		return 1;

	} else if (match_count == raid_seg->area_count) {
		log_error("Unable to remove all PVs from %s at once.",
			  display_lvname(lv));
		return 0;

	} else if (raid_seg->segtype->parity_devs &&
		   (match_count > raid_seg->segtype->parity_devs)) {
		log_error("Unable to replace more than %u PVs from (%s) %s",
			  raid_seg->segtype->parity_devs,
			  lvseg_name(raid_seg),
			  display_lvname(lv));
		return 0;

	} else if (seg_is_raid10_near(raid_seg)) {
		uint32_t i, rebuilds_per_group = 0;
		/* FIXME: We only support 2-way data_copies in RAID10 currently */
		uint32_t copies;

		copies = raid_seg->data_copies;

		for (i = 0; i < raid_seg->area_count * copies; i++) {
			s = i % raid_seg->area_count;
			if (!(i % copies))
				rebuilds_per_group = 0;
			if (lv_is_on_pvs(seg_lv(raid_seg, s), remove_pvs) ||
			    lv_is_on_pvs(seg_metalv(raid_seg, s), remove_pvs) ||
			    lv_is_virtual(seg_lv(raid_seg, s)) ||
			    lv_is_virtual(seg_metalv(raid_seg, s)))
				rebuilds_per_group++;
			if (rebuilds_per_group >= copies) {
				log_error("Unable to replace all the devices "
					  "in a RAID10 mirror group.");
				return 0;
			}
		}
#if 0
	}
#else
	} else if (seg_is_any_raid10(raid_seg)) {
		/* HM FIXME: do the raid10_{far,offset} thing */
		return 0;
	}
#endif
	
	/* Prevent any PVs holding image components from being used for allocation */
	if (!_avoid_pvs_with_other_images_of_lv(lv, allocate_pvs)) {
		log_error("Failed to prevent PVs holding image components "
			  "from lv %s being used for allocation.",
			  display_lvname(lv));
		return 0;
	}

	/*
	 * Allocate the new image components first
	 * - This makes it easy to avoid all currently used devs
	 * - We can immediately tell if there is enough space
	 *
	 * - We need to change the LV names when we insert them.
	 */
	while (!_alloc_image_components(lv, allocate_pvs, match_count,
				        &new_meta_lvs, &new_data_lvs)) {
		if (!(lv->status & PARTIAL_LV)) {
			log_error("LV %s in not partial.", display_lvname(lv));
			return 0;
		}

		/*
		 * We failed allocating all required devices so
		 * we'll try less devices; we must set partial_activation
		 */
		lv->vg->cmd->partial_activation = 1;

		/* This is a repair, so try to do better than all-or-nothing */
		if (match_count > 0 && !partial_segment_removed) {
			log_error("Failed to replace %u devices.", match_count);
			match_count--;
			log_error("Attempting to replace %u instead.", match_count);

		} else if (!partial_segment_removed) {
			/*
			 * match_count = 0
			 *
			 * We are down to the last straw.  We can only hope
			 * that a failed PV is just one of several PVs in
			 * the image; and if we extract the image, there may
			 * be enough room on the image's other PVs for a
			 * reallocation of the image.
			 */
			if (!_remove_partial_multi_segment_image(lv, remove_pvs))
				return_0;

			match_count = 1;
			partial_segment_removed = 1;

		} else {

			log_error("Failed to allocate replacement images for %s",
				  display_lvname(lv));
			return 0;
		}
	}

	/*
	 * Remove the old images
	 * - If we did this before the allocate, we wouldn't have to rename
	 *   the allocated images, but it'd be much harder to avoid the right
	 *   PVs during allocation.
	 *
	 * - If this is a repair and we were forced to call
	 *   _remove_partial_multi_segment_image, then the remove_pvs list
	 *   is no longer relevant - _raid_extract_images is forced to replace
	 *   the image with the error target.  Thus, the full set of PVs is
	 *   supplied - knowing that only the image with the error target
	 *   will be affected.
	 */
	if (!_raid_extract_images(lv, raid_seg->area_count - match_count,
				  partial_segment_removed ?
				  &lv->vg->pvs : remove_pvs, 0 /* Don't shift */,
				  &old_lvs, &old_lvs)) {
		log_error("Failed to remove the specified images from %s",
			  display_lvname(lv));
		return 0;
	}

	/*
	 * Now that they are extracted and visible, make the system aware
	 * of their new names.
	 */
	dm_list_iterate_items(lvl, &old_lvs)
		if (!activate_lv_excl_local(lv->vg->cmd, lvl->lv))
			return_0;

	/*
	 * Skip metadata operation normally done to clear the metadata sub-LVs.
	 *
	 * The LV_REBUILD flag is set on the new sub-LVs,
	 * so they will be rebuilt and we don't need to clear the metadata dev.
	 *
	 * Insert new allocated image component pairs into now empty area slots.
	 */
	for (s = 0; s < raid_seg->area_count; s++) {
		sd = s + raid_seg->area_count;

		if ((seg_type(raid_seg, s) == AREA_UNASSIGNED) &&
		    (seg_metatype(raid_seg, s) == AREA_UNASSIGNED)) {
			if (!_generate_name_and_set_segment(lv, s, s,  &new_meta_lvs, tmp_names) ||
			    !_generate_name_and_set_segment(lv, s, sd, &new_data_lvs, tmp_names))
				return 0;
		} 
	}

	/* This'll reset the rebuild flags passed to the kernel as well */
	if (!_lv_update_and_reload_origin_eliminate_lvs(lv, &old_lvs))
		return_0;

	/* Update new sub-LVs to correct name and clear REBUILD flag in-kernel and in metadata */
	for (s = 0; s < raid_seg->area_count; s++) {
		sd = s + raid_seg->area_count;
		if (tmp_names[s]) {
			seg_metalv(raid_seg, s)->name = tmp_names[s];
			seg_lv(raid_seg, s)->name = tmp_names[sd];
		}
	}

	/* HM FIXME: doesn't start repair */
	if (!lv_update_and_reload_origin(lv))
		return_0;

	return _lv_cond_repair(lv);
}

/* Replace any partial data and metadata LVs with error segments */
int lv_raid_remove_missing(struct logical_volume *lv)
{
	uint32_t s;
	struct lv_segment *seg = first_seg(lv);

	if (!(lv->status & PARTIAL_LV)) {
		log_error(INTERNAL_ERROR "%s is not a partial LV",
			  display_lvname(lv));
		return 0;
	}

	if (!archive(lv->vg))
		return_0;

	log_debug("Attempting to remove missing devices from %s LV, %s",
		  lvseg_name(seg), lv->name);

	/*
	 * FIXME: Make sure # of compromised components will not affect RAID
	 */
	for (s = 0; s < seg->area_count; s++)
		if (!_replace_lv_with_error_segment(seg_lv(seg, s)) ||
		    !_replace_lv_with_error_segment(seg_metalv(seg, s)))
			return 0;

	if (!lv_update_and_reload(lv))
		return_0;

	return 1;
}

/* Return 1 if a partial raid LV can be activated redundantly */
static int _partial_raid_lv_is_redundant(const struct logical_volume *lv)
{
	struct lv_segment *raid_seg = first_seg(lv);
	uint32_t copies;
	uint32_t i, s, rebuilds_per_group = 0;
	uint32_t failed_components = 0;

	if (seg_is_raid10(raid_seg)) {
		/* FIXME: We only support 2-way data_copies in RAID10 currently */
		copies = 2;
		for (i = 0; i < raid_seg->area_count * copies; i++) {
			s = i % raid_seg->area_count;

			if (!(i % copies))
				rebuilds_per_group = 0;

			if ((seg_lv(raid_seg, s)->status & PARTIAL_LV) ||
			    (seg_metalv(raid_seg, s)->status & PARTIAL_LV) ||
			    lv_is_virtual(seg_lv(raid_seg, s)) ||
			    lv_is_virtual(seg_metalv(raid_seg, s)))
				rebuilds_per_group++;

			if (rebuilds_per_group >= copies) {
				log_verbose("An entire mirror group has failed in %s.",
					    display_lvname(lv));
				return 0;	/* Insufficient redundancy to activate */
			}
		}

		return 1; /* Redundant */
	}

	for (s = 0; s < raid_seg->area_count; s++) {
		if ((seg_lv(raid_seg, s)->status & PARTIAL_LV) ||
		    (seg_metalv(raid_seg, s)->status & PARTIAL_LV) ||
		    lv_is_virtual(seg_lv(raid_seg, s)) ||
		    lv_is_virtual(seg_metalv(raid_seg, s)))
			failed_components++;
	}

	if (failed_components && seg_is_any_raid0(raid_seg)) {
		log_verbose("No components of raid LV %s may fail",
			    display_lvname(lv));
		return 0;

	} else if (failed_components == raid_seg->area_count) {
		log_verbose("All components of raid LV %s have failed.",
			    display_lvname(lv));
		return 0;	/* Insufficient redundancy to activate */
	} else if (raid_seg->segtype->parity_devs &&
		   (failed_components > raid_seg->segtype->parity_devs)) {
		log_verbose("More than %u components from %s %s have failed.",
			    raid_seg->segtype->parity_devs,
			    lvseg_name(raid_seg),
			    display_lvname(lv));
		return 0;	/* Insufficient redundancy to activate */
	}

	return 1;
}

/* Sets *data to 1 if the LV cannot be activated without data loss */
static int _lv_may_be_activated_in_degraded_mode(struct logical_volume *lv, void *data)
{
	int *not_capable = (int *)data;
	uint32_t s;
	struct lv_segment *seg;

	if (*not_capable)
		return 1;	/* No further checks needed */

	if (!(lv->status & PARTIAL_LV))
		return 1;

	if (lv_is_raid(lv)) {
		*not_capable = !_partial_raid_lv_is_redundant(lv);
		return 1;
	}

	/* Ignore RAID sub-LVs. */
	if (lv_is_raid_type(lv))
		return 1;

	dm_list_iterate_items(seg, &lv->segments)
		for (s = 0; s < seg->area_count; s++)
			if (seg_type(seg, s) != AREA_LV) {
				log_verbose("%s contains a segment incapable of degraded activation",
					    display_lvname(lv));
				*not_capable = 1;
			}

	return 1;
}

/* Check if @clv supported egraded activation */
int partial_raid_lv_supports_degraded_activation(const struct logical_volume *clv)
{
	int not_capable = 0;
	struct logical_volume * lv = (struct logical_volume *)clv; /* drop const */

	if (!_lv_may_be_activated_in_degraded_mode(lv, &not_capable) || not_capable)
		return_0;

	if (!for_each_sub_lv(lv, _lv_may_be_activated_in_degraded_mode, &not_capable)) {
		log_error(INTERNAL_ERROR "for_each_sub_lv failure.");
		return 0;
	}

	return !not_capable;
}

/* raid10_far helper: ensure consistent image LVs have been passed in for @seg */
static int _seg_images_are_divisible_by_data_copies(struct lv_segment *seg)
{
	uint32_t s;
	uint32_t len = seg_lv(seg, 0)->le_count;

	for (s = 0; s < seg->area_count; s++) {
		if (seg_type(seg, s) != AREA_LV) {
			log_error(INTERNAL_ERROR "raid10_far segment of LV %s missing image LV!",
				  display_lvname(seg->lv));
			return 0;
		}

		if (seg_lv(seg, s)->le_count != len) {
			log_error(INTERNAL_ERROR "raid10_far image length of LV %s differ in size!",
				  display_lvname(seg->lv));
			return 0;
		}

		if (seg_lv(seg, s)->le_count % seg->data_copies) {
			log_error(INTERNAL_ERROR "raid10_far image length of LV %s not divisible by #data_copies!",
				  display_lvname(seg->lv));
			return 0;
		}
	}

	return 1;
}

/* raid10_far helper: split up all data image sub LVs of @lv from @start LE till @end LE in @split_len increments */
static int _split_lv_data_images(struct logical_volume *lv,
				 uint32_t start, uint32_t end, uint32_t split_len)
{
	uint32_t s;
	struct lv_segment *seg = first_seg(lv);

	for (s = 0; s < seg->area_count; s++) {
		uint32_t le;
		struct logical_volume *slv = seg_lv(seg, s);

		/* Split the image up */
		for (le = start; le < end; le += split_len)
			if (!lv_split_segment(slv, le))
				return_0;
	}

	return 1;
}

/*
 * Reorder segments for @extents length in @lv;
 * @extend flag indicates extension/reduction request.
 *
 * raid10_far arranges stripe zones with differing data block rotation
 * one after the other and data_copies across them.
 * In order to resize those, we have to split them up by # data copies
 * and reorder the split sgements.
 */
int lv_raid10_far_reorder_segments(struct logical_volume *lv, uint32_t extents, int extend)
{
	uint32_t le, s;
	struct logical_volume *slv;
	struct lv_segment *seg, *raid_seg = first_seg(lv);

	if (!extents) {
		log_error(INTERNAL_ERROR "Called on LV %s for 0 extents!", display_lvname(lv));
		return 0;
	}

	/* We may only reorder in case of raid10 far */
	if (!seg_is_raid10_far(raid_seg)) {
		log_error(INTERNAL_ERROR "Called on non-raid10_far LV %s with type %s!",
			  raid_seg->segtype->name, display_lvname(lv));
		return 0;
	}

PFLA("extents=%u lv->le_count=%u raid_seg->area_len=%u", extents, lv->le_count, raid_seg->area_len);
	/* Check properties of new segment and previous segment for compaitbility */
	if (!_seg_images_are_divisible_by_data_copies(raid_seg))
		return 0;
PFL();
	if (extend) {
		uint32_t new_split_len, prev_le_count, prev_split_len;

		/* If this is a new LV -> no need to reorder */
		if (extents == lv->le_count)
			return 1;

		/*
		 * We've got new extemts added to the image lvs which
		 * are in the wrong place; got to split them up to insert
		 * the split ones into the previous raid10_far ones.
		 */
		/* Ensure proper segment boundaries so that we can move segments */

		/* Split segments of all image LVs for reordering */
		prev_le_count = lv_raid_rimage_extents(lv->le_count - extents, raid_seg->area_count, raid_seg->data_copies);
		prev_split_len = prev_le_count / raid_seg->data_copies;
		if (!_split_lv_data_images(lv, prev_split_len, prev_le_count, prev_split_len))
			return 0;

		/* Split the newly allocated part of the images up */
		slv = seg_lv(raid_seg, 0);
		new_split_len = (slv->le_count - prev_le_count) / raid_seg->data_copies;
		if (!_split_lv_data_images(lv, prev_le_count, slv->le_count, new_split_len))
			return 0;
PFL();
		/*
		 * Reorder segments of the image LVs so that the split off #data_copies
		 * segmentsof the new allocation get moved to the ends of the split off
		 * previous ones.
		 *
		 * E.g. with 3 data copies before/after reordering an image LV:
		 *
		 * P1, P2, P3, N1, N2, N3 -> P1, N1, P2, N2, P3, N3
		 */
		for (s = 0; s < raid_seg->area_count; s++) {
			uint32_t le2;
			struct lv_segment *seg2;

			slv = seg_lv(raid_seg, s);
			for (le = prev_split_len, le2 = prev_le_count + new_split_len;
			     le2 < slv->le_count;
			     le += prev_split_len, le2 += new_split_len) {
				seg  = find_seg_by_le(slv, le);
				seg2 = find_seg_by_le(slv, le2);
				dm_list_move(seg->list.n, &seg2->list);
			}
		}

	/*
	 * Reduce...
	 */
	} else {
		uint32_t reduction, split_len;

		/* Only reorder in case of partial reduction */
		if (extents >= raid_seg->len)
			return 1;

		/* Ensure proper segment boundaries so that we can move segments */
		/* Split segments of all image LVs for reordering */
		slv = seg_lv(raid_seg, 0);
		reduction = extents / raid_seg->area_count;
		split_len = slv->le_count / raid_seg->data_copies;
		if (!_split_lv_data_images(lv, split_len - reduction, slv->le_count, split_len) ||
		    !_split_lv_data_images(lv, split_len, slv->le_count, split_len))
			return 0;
PFL();
		/* Reorder split segments of all image LVs to have those to reduce at the end */
		for (s = 0; s < raid_seg->area_count; s++) {
			slv = seg_lv(raid_seg, s);
			for (le = split_len - reduction; le < slv->le_count; le += split_len) {
				seg = find_seg_by_le(slv, le);
				dm_list_move(&slv->segments, &seg->list);
			}
		}
PFL();
	}

	/* Correct segments start logical extents and length */
	_lv_set_image_lvs_start_les(lv);

	return 1;
}

/*
 * Helper: create a raid01 (data_copies on top of stripes) LV
 */
int lv_create_raid01(struct logical_volume *lv, const struct segment_type *segtype,
		     unsigned data_copies, unsigned stripes,
		     unsigned stripe_size, unsigned region_size,
		     unsigned extents, struct dm_list *allocate_pvs)
{
	uint64_t status = RAID_IMAGE | LVM_READ | LVM_WRITE;
	struct lv_segment *raid1_seg;
	struct segment_type *image_segtype;
	struct volume_group *vg = lv->vg;

PFLA("mrrors=%u stripes=%u", data_copies, stripes);
	if (data_copies < 2 || stripes < 2)
		return 0;

	if (!(image_segtype = get_segtype_from_string(vg->cmd, SEG_TYPE_NAME_STRIPED)))
		return_0;

	if (!archive(vg))
		return_0;

	if (!replace_lv_with_error_segment(lv))
		return_0;

	/* Create the one top-level segment for our raid1 split LV and add it to the LV  */
	if (!(raid1_seg = alloc_lv_segment(segtype, lv, 0 /* le */, extents /* len */,
					   0 /* reshape_len */, status | RAID,
					   0 /* stripe_size */, NULL,
					   data_copies, extents,
					   data_copies, 0, region_size, 0, NULL))) {
		log_error("Failed to create raid1 top-level segment for %s %s",
			  segtype->name, display_lvname(lv));
		return_0;
	}

	_check_and_init_region_size(lv);
	dm_list_init(&lv->segments);
	dm_list_add(&lv->segments, &raid1_seg->list);

	if (!_lv_create_raid01_image_lvs(lv, image_segtype, extents,
					 stripe_size, 0, data_copies, allocate_pvs))
		return 0;

PFLA("raid1_seg->len=%u raid1_seg->area_len=%u", raid1_seg->len, raid1_seg->area_len);
	lv->le_count = raid1_seg->len;
	lv->size = raid1_seg->len * lv->vg->extent_size;
PFL();
	raid1_seg->meta_areas = NULL; /* Reset to force rmeta device creation in raid01 segment */

	return _alloc_and_add_rmeta_devs_for_lv(lv);
}
