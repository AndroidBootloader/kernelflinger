/*
 * Copyright (c) 2014, Intel Corporation
 * All rights reserved.
 *
 * Authors: Sylvain Chouleur <sylvain.chouleur@intel.com>
 *          Jeremy Compostella <jeremy.compostella@intel.com>
 *          Jocelyn Falempe <jocelyn.falempe@intel.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <efi.h>
#include <efilib.h>
#include <lib.h>
#include "uefi_utils.h"
#include "gpt.h"
#include "gpt_bin.h"
#include "storage.h"

#define PROTECTIVE_MBR 0xEE
#define GPT_SIGNATURE "EFI PART"

struct gpt_header {
	char signature[8];
	UINT32 revision;
	UINT32 size;
	UINT32 header_crc32;
	UINT32 reserved_zero;
	UINT64 my_lba;
	UINT64 alternate_lba;
	UINT64 first_usable_lba;
	UINT64 last_usable_lba;
	EFI_GUID disk_uuid;
	UINT64 entries_lba;
	UINT32 number_of_entries;
	UINT32 size_of_entry;
	UINT32 entries_crc32;
	/* Remainder of sector is reserved and should be 0 */
} __attribute__((packed));

struct legacy_partition {
	UINT8	status;
	UINT8	f_head;
	UINT8	f_sect;
	UINT8	f_cyl;
	UINT8	type;
	UINT8	l_head;
	UINT8	l_sect;
	UINT8	l_cyl;
	UINT32	f_lba;
	UINT32	num_sect;
} __attribute__((packed));

struct mbr_chs {
	uint8_t head;
	uint8_t sector; /* sector in bits 5-0, 7-6 hi bits of cyl */
	uint8_t cylinder;
} __attribute__((__packed__));

struct mbr_entry {
	uint8_t status;
	struct mbr_chs first_chs;
	uint8_t type;
	struct mbr_chs last_chs;
	uint32_t first_lba;
	uint32_t lba_count;
} __attribute__((__packed__));

struct mbr {
	uint32_t disk_sig;
	uint16_t reserved;
	struct mbr_entry entries[4];
	uint16_t sig;
} __attribute__((__packed__));

struct gpt_disk {
	EFI_BLOCK_IO *bio;
	EFI_DISK_IO *dio;
	EFI_HANDLE handle;
	BOOLEAN label_prefix_removed;
	logical_unit_t log_unit;
	struct gpt_header gpt_hd;
	struct gpt_partition *partitions;
};

/* Allow to scan and flash only one disk at a time
 * this disk could be emmc user area or emmc gpp */
static struct gpt_disk sdisk;

static EFI_STATUS calculate_crc32(void *data, UINTN size, UINT32 *crc)
{
	EFI_STATUS ret;

	ret = uefi_call_wrapper(BS->CalculateCrc32, 3, data, size, crc);
	if (EFI_ERROR(ret))
		efi_perror(ret, L"CalculateCrc32 failed");
	return ret;
}

static EFI_STATUS set_header_crc32(struct gpt_header *gh)
{
	UINT32 crc;
	EFI_STATUS ret;

	gh->header_crc32 = 0;
	ret = calculate_crc32(gh, sizeof(struct gpt_header), &crc);
	gh->header_crc32 = crc;
	return ret;
}

static EFI_STATUS read_gpt_header(struct gpt_disk *disk)
{
	EFI_STATUS ret;

	ret = uefi_call_wrapper(disk->dio->ReadDisk, 5, disk->dio, disk->bio->Media->MediaId, disk->bio->Media->BlockSize, sizeof(disk->gpt_hd), (VOID *)&disk->gpt_hd);
	if (EFI_ERROR(ret))
		efi_perror(ret, L"Failed to read disk for GPT header");

	return ret;
}

static BOOLEAN is_gpt_device(struct gpt_header *gpt)
{
	return CompareMem(gpt->signature, GPT_SIGNATURE, sizeof(gpt->signature)) == 0;
}

static EFI_STATUS read_gpt_partitions(struct gpt_disk *disk)
{
	EFI_STATUS ret;
	UINTN offset;
	UINTN size;

	offset = disk->bio->Media->BlockSize * disk->gpt_hd.entries_lba;
	size = disk->gpt_hd.number_of_entries * disk->gpt_hd.size_of_entry;

	disk->partitions = AllocatePool(size);
	if (!disk->partitions) {
		error(L"Failed to allocate %d bytes for partitions", size);
		return EFI_OUT_OF_RESOURCES;
	}

	ret = uefi_call_wrapper(disk->dio->ReadDisk, 5, disk->dio, disk->bio->Media->MediaId, offset, size, disk->partitions);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to read GPT partitions");
		goto free_partitions;
	}
	return ret;

free_partitions:
	FreePool(disk->partitions);
	disk->partitions = NULL;
	return ret;
}

static EFI_STATUS gpt_prepare_disk(EFI_HANDLE handle, struct gpt_disk *disk)
{
	EFI_STATUS ret;

	ret = uefi_call_wrapper(BS->HandleProtocol, 3, handle, &BlockIoProtocol, (VOID *)&disk->bio);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to get block io protocol");
		return ret;
	}

	if (disk->bio->Media->LogicalPartition != 0)
		return EFI_NOT_FOUND;

	if (disk->bio->Media->RemovableMedia)
		return EFI_NOT_FOUND;

	ret = uefi_call_wrapper(BS->HandleProtocol, 3, handle, &DiskIoProtocol, (VOID *)&disk->dio);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to get disk io protocol");
		return ret;
	}

	ret = read_gpt_header(disk);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to read GPT header");
		return ret;
	}
	return ret;
}

/* Gmin adds the "android_" prefix to the partition label.  Most of
   the fastboot command relies on the partition name/label.  The
   following functions get rid of this prefix and put it if previously
   removed.  */
const CHAR16 *ANDROID_PREFIX = L"android_";

static EFI_STATUS gpt_remove_prefix(void)
{
	UINTN prefix_len = StrLen(ANDROID_PREFIX);
	BOOLEAN removed = FALSE;
	BOOLEAN not_removed = FALSE;
	UINTN p;

	if (sdisk.label_prefix_removed)
		return EFI_SUCCESS;

	for (p = 0; p < sdisk.gpt_hd.number_of_entries; p++) {
		struct gpt_partition *part;

		part = &sdisk.partitions[p];
		if (!CompareGuid(&part->type, &NullGuid))
			continue;

		if (!StrnCmp(part->name, ANDROID_PREFIX, prefix_len)) {
			if (not_removed)
				goto error;
			CopyMem(part->name, &part->name[prefix_len],
				sizeof(part->name) - (prefix_len * sizeof(CHAR16)));
			removed = TRUE;
			continue;
		}
		if (removed == TRUE)
			goto error;

		not_removed = TRUE;
	}

	sdisk.label_prefix_removed = TRUE;
	return EFI_SUCCESS;
error:
	error(L"Not all the partition have the '%s' prefix", ANDROID_PREFIX);
	return EFI_INVALID_PARAMETER;
}

static void gpt_put_prefix_back(void)
{
	UINTN prefix_len = StrLen(ANDROID_PREFIX);
	struct gpt_partition save;
	UINTN p;

	if (!sdisk.label_prefix_removed)
		return;

	for (p = 0; p < sdisk.gpt_hd.number_of_entries; p++) {
		struct gpt_partition *part;

		part = &sdisk.partitions[p];
		if (!CompareGuid(&part->type, &NullGuid))
			continue;

		CopyMem(save.name, part->name, sizeof(part->name));
		CopyMem(&part->name[prefix_len], save.name,
			sizeof(part->name) - (prefix_len * sizeof(CHAR16)));
		CopyMem(part->name, ANDROID_PREFIX, prefix_len * sizeof(CHAR16));
	}

	sdisk.label_prefix_removed = FALSE;
}

static EFI_STATUS gpt_list_partition_on_disk(struct gpt_disk *disk)
{
	EFI_STATUS ret;

	if (!is_gpt_device(&disk->gpt_hd))
		return EFI_NOT_FOUND;
	ret = read_gpt_partitions(disk);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to read GPT partitions");
		return ret;
	}
	ret = gpt_remove_prefix();
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to remove prefix of partition label");
		return ret;
	}

	return EFI_SUCCESS;
}

/* Given the logical unit, find the disk and caches
 * information into the global sdisk variable */
static EFI_STATUS gpt_cache_partition(logical_unit_t log_unit)
{
	EFI_STATUS ret;
	EFI_HANDLE *handles;
	UINTN nb_handle = 0;
	UINTN i;
	BOOLEAN found = FALSE;
	EFI_DEVICE_PATH *device_path;

	/* if  already cached, return */
	if (sdisk.dio && sdisk.log_unit == log_unit)
		return EFI_SUCCESS;

	ret = uefi_call_wrapper(BS->LocateHandleBuffer, 5, ByProtocol, &BlockIoProtocol, NULL, &nb_handle, &handles);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to locate Block IO Protocol");
		return ret;
	}
	debug(L"Found %d block io protocols", nb_handle);

	for (i = 0; i < nb_handle && !found; i++) {
		/* Check if the logical unit match the requested one */
		device_path = DevicePathFromHandle(handles[i]);
		ret = storage_check_logical_unit(device_path, log_unit);
		if (EFI_ERROR(ret))
			continue;

		ZeroMem(&sdisk, sizeof(sdisk));
		ret = gpt_prepare_disk(handles[i], &sdisk);
		if (EFI_ERROR(ret))
			continue;
		debug(L"Found disk as block io %d for logical unit %d", i, log_unit);

		sdisk.handle = handles[i];
		sdisk.log_unit = log_unit;
		found = TRUE;
	}
	if (!found) {
		error(L"No disk found for logical unit %d", log_unit);
		ret = EFI_NOT_FOUND;
		goto free_handles;
	}

	/* only system's gpt partitions will be flashed through fastboot */
	if (log_unit != LOGICAL_UNIT_USER)
		return EFI_SUCCESS;

	ret = gpt_list_partition_on_disk(&sdisk);
	/* ignore if there are no gpt partition on the system disk */
	if (EFI_ERROR(ret)) {
		ZeroMem(&sdisk.gpt_hd, sizeof(struct gpt_header));
	}
	ret = EFI_SUCCESS;

free_handles:
	FreePool(handles);
	return ret;
}

void gpt_free_cache(void)
{
	if (sdisk.partitions)
		FreePool(sdisk.partitions);
	ZeroMem(&sdisk, sizeof(sdisk));
}

EFI_STATUS gpt_refresh(void)
{
	EFI_STATUS ret;

	ret = uefi_call_wrapper(sdisk.bio->FlushBlocks, 1, sdisk.bio);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to flush block io interface");
		return ret;
	}
	ret = uefi_call_wrapper(BS->ReinstallProtocolInterface, 4, sdisk.handle, &BlockIoProtocol, sdisk.bio, sdisk.bio);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to Reinstall block io interface on System disk");
		return ret;
	}
	/* invalid gpt cache to force to get new handle next time */
	gpt_free_cache();

	return EFI_SUCCESS;
}

EFI_STATUS gpt_get_root_disk(struct gpt_partition_interface *gpart, logical_unit_t log_unit)
{
	EFI_STATUS ret;

	ret = gpt_cache_partition(log_unit);
	if (EFI_ERROR(ret))
		return ret;

	gpart->part.starting_lba = 0;
	gpart->part.ending_lba = sdisk.bio->Media->LastBlock;
	gpart->bio = sdisk.bio;
	gpart->dio = sdisk.dio;

	return EFI_SUCCESS;
}

static struct gpt_partition *gpt_find_partition(CHAR16 *label)
{
	UINTN p;

	for (p = 0; p < sdisk.gpt_hd.number_of_entries; p++) {
		struct gpt_partition *part;

		part = &sdisk.partitions[p];
		if (!CompareGuid(&part->type, &NullGuid) || StrCmp(part->name, label))
			continue;

		debug(L"Found label %s in partition %d", label, p);
		return part;
	}

	return NULL;
}

EFI_STATUS gpt_get_partition_by_label(CHAR16 *label, struct gpt_partition_interface *gpart, logical_unit_t log_unit)
{
	struct gpt_partition *part;
	EFI_STATUS ret;

	ret = gpt_cache_partition(log_unit);
	if (EFI_ERROR(ret))
		return ret;

	part = gpt_find_partition(label);
	if (part) {
		CopyMem(&gpart->part, part, sizeof(*part));
		gpart->bio = sdisk.bio;
		gpart->dio = sdisk.dio;
		gpart->handle = sdisk.handle;
		return EFI_SUCCESS;
	}

	if (!StrCmp(label, L"userdata"))
		return gpt_get_partition_by_label(L"data", gpart, log_unit);

	return EFI_NOT_FOUND;
}

EFI_STATUS gpt_list_partition(struct gpt_partition_interface **gpartlist, UINTN *part_count, logical_unit_t log_unit)
{
	EFI_STATUS ret;
	UINTN p;

	ret = gpt_cache_partition(log_unit);
	if (EFI_ERROR(ret))
		return ret;

	*part_count = 0;
	if (!sdisk.gpt_hd.number_of_entries)
		return EFI_SUCCESS;

	*gpartlist = AllocatePool(sdisk.gpt_hd.number_of_entries * sizeof(struct gpt_partition_interface));
	if (!*gpartlist)
		return EFI_OUT_OF_RESOURCES;

	for (p = 0; p < sdisk.gpt_hd.number_of_entries; p++) {
		struct gpt_partition *part;
		struct gpt_partition_interface *parti;

		part = &sdisk.partitions[p];
		if (!CompareGuid(&part->type, &NullGuid) || !part->name[0])
			continue;

		parti = &(*gpartlist)[(*part_count)];
		parti->bio = sdisk.bio;
		parti->dio = sdisk.dio;
		CopyMem(&parti->part, part, sizeof(*part));
		(*part_count)++;
	}
	return EFI_SUCCESS;
}

#define GPT_REVISION 0x00010000
#define GPT_ENTRIES 128
#define GPT_ENTRY_SIZE 128

static void gpt_new(struct gpt_header *gh, UINTN start_lba, UINTN blocksize, UINTN lastblock)
{
	UINTN gpt_size;

	ZeroMem(gh, sizeof(struct gpt_header));
	CopyMem(gh->signature, "EFI PART", 8);

	gh->revision = GPT_REVISION;
	gh->size = sizeof(*gh);

	/* All the math assumes that total size of pentries is
	 * some multiple of sector size */
	gh->number_of_entries = GPT_ENTRIES;
	gh->size_of_entry = GPT_ENTRY_SIZE;
	gpt_size = 1 + (gh->number_of_entries * gh->size_of_entry / blocksize);
	/* if start_lba is forced, use it, otherwise start at 1 MiB */
	if (start_lba && start_lba > 2 + gpt_size)
		gh->first_usable_lba = start_lba;
	else
		gh->first_usable_lba = MiB / blocksize;
	gh->last_usable_lba = ALIGN_DOWN(lastblock - (gpt_size), (MiB / blocksize)) - 1;

	debug(L"first usable lba %ld, last usable lba %ld",
	      gh->first_usable_lba, gh->last_usable_lba);
	/* TODO generate unique UUID for disk */
}

/*
 * check that the list of partitions to write to the gpt table
 * is well formated, fit inside the disk, and calculate the size
 * of the partition with "-1" length if any
 */
static EFI_STATUS gpt_check_partition_list(UINTN part_count, struct gpt_bin_part *gbp)
{
	UINTN i;
	UINT64 totsize = 0;
	UINT64 disksize;
	INTN part_data = -1;

	for (i = 0; i < part_count; i++) {
		if (gbp[i].length == 0 || gbp[i].length < -1) {
			error(L"Wrong length for partition %d", i);
			return EFI_INVALID_PARAMETER;
		}
		if (gbp[i].length == -1) {
			if (part_data >= 0) {
				error(L"More than 1 partition has -1 length %d", i);
				return EFI_INVALID_PARAMETER;
			}
			part_data = i;
			continue;
		}
		totsize += gbp[i].length;
	}
	disksize = ((sdisk.gpt_hd.last_usable_lba + 1 - sdisk.gpt_hd.first_usable_lba) * sdisk.bio->Media->BlockSize) / MiB;

	if (totsize > disksize) {
		error(L"partitions are bigger than the disk, partitions %ld MiB disk %ld MiB", totsize, disksize);
		return EFI_INVALID_PARAMETER;
	}
	gbp[part_data].length = disksize - totsize;
	return EFI_SUCCESS;
}

static struct gpt_partition *gpt_fill_entries(UINTN part_count, struct gpt_bin_part *gbp)
{
	struct gpt_partition *gp;
	UINT64 start_lba;
	UINTN i;

	gp = AllocateZeroPool(sdisk.gpt_hd.number_of_entries * sdisk.gpt_hd.size_of_entry);
	if (!gp)
		return NULL;

	/* align on MiB boundaries ??? */
	start_lba = sdisk.gpt_hd.first_usable_lba;

	for (i = 0; i < part_count; i++) {
		CopyMem(&gp[i].name, &gbp[i].label, sizeof(gp[i].name));
		CopyMem(&gp[i].type, &gbp[i].type, sizeof(EFI_GUID));
		CopyMem(&gp[i].unique, &gbp[i].uuid, sizeof(EFI_GUID));
		gp[i].starting_lba = start_lba;
		gp[i].ending_lba = start_lba - 1 + gbp[i].length * (MiB / sdisk.bio->Media->BlockSize);
		start_lba = gp[i].ending_lba + 1;
		debug(L"partition %s, start %ld, end %ld", gp[i].name, gp[i].starting_lba, gp[i].ending_lba);
	}
	return gp;
}

static EFI_STATUS gpt_write_mbr(void)
{
	struct mbr mbr;
	EFI_STATUS ret;

	/* Write protective MBR */
	ZeroMem(&mbr, sizeof(mbr));
	mbr.sig = 0xAA55;
	mbr.entries[0].type = PROTECTIVE_MBR;
	mbr.entries[0].first_lba = 1;
	if (sdisk.bio->Media->LastBlock > 0xFFFFFFFFULL)
		mbr.entries[0].lba_count = 0xFFFFFFFFULL;
	else
		mbr.entries[0].lba_count = sdisk.bio->Media->LastBlock;

	ret = uefi_call_wrapper(sdisk.dio->WriteDisk, 5, sdisk.dio, sdisk.bio->Media->MediaId,
				440, sizeof(struct mbr), &mbr);
	if (EFI_ERROR(ret))
		error(L"Couldn't write MBR");

	return ret;
}

static EFI_STATUS gpt_write_table_to_disk(struct gpt_header *gh)
{
	UINT64 entries_offset, header_offset, entries_size;
	EFI_STATUS ret;

	entries_size = gh->number_of_entries * gh->size_of_entry;
	header_offset = gh->my_lba * sdisk.bio->Media->BlockSize;
	entries_offset = gh->entries_lba * sdisk.bio->Media->BlockSize;

	ret = uefi_call_wrapper(sdisk.dio->WriteDisk, 5, sdisk.dio, sdisk.bio->Media->MediaId,
				header_offset, sizeof(struct gpt_header), gh);
	if (EFI_ERROR(ret)) {
		error(L"Couldn't write GPT header");
		return ret;
	}

	ret = uefi_call_wrapper(sdisk.dio->WriteDisk, 5, sdisk.dio, sdisk.bio->Media->MediaId,
				entries_offset, entries_size,
				sdisk.partitions);
	if (EFI_ERROR(ret))
		error(L"Couldn't write GPT entries array");

	return ret;
}

static EFI_STATUS gpt_write_partition_tables(void)
{
	EFI_STATUS ret;
	UINT64 entries_size;
	struct gpt_header *gh;
	struct gpt_header *gh_backup;
	UINT32 crc;

	gpt_put_prefix_back();

	gh = &sdisk.gpt_hd;

	entries_size = gh->number_of_entries * gh->size_of_entry;
	gh->my_lba = 1;
	gh->alternate_lba = sdisk.bio->Media->LastBlock;
	gh->entries_lba = 2;

	ret = calculate_crc32(sdisk.partitions, entries_size, &crc);
	if (EFI_ERROR(ret))
		return ret;

	gh->entries_crc32 = crc;

	ret = set_header_crc32(gh);
	if (EFI_ERROR(ret))
		return ret;

	debug(L"Write first GPT Header at %d", gh->my_lba);
	ret = gpt_write_table_to_disk(gh);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to write primary GPT header");
		return ret;
	}

	gh_backup = AllocatePool(sizeof(struct gpt_header));
	if (!gh_backup) {
		error(L"Cannot allocate alternate GPT header");
		return EFI_OUT_OF_RESOURCES;
	}

	CopyMem(gh_backup, gh, sizeof(struct gpt_header));

	gh_backup->my_lba = gh->alternate_lba;
	gh_backup->alternate_lba = gh->my_lba;
	gh_backup->entries_lba = gh_backup->my_lba - entries_size / sdisk.bio->Media->BlockSize;

	ret = set_header_crc32(gh_backup);
	if (EFI_ERROR(ret))
		return ret;

	debug(L"Write alternate GPT Header at %d", gh_backup->my_lba);
	ret = gpt_write_table_to_disk(gh_backup);
	FreePool(gh_backup);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to write alternate GPT header");
		return ret;
	}
	debug(L"Write protective MBR");
	ret = gpt_write_mbr();
	if (EFI_ERROR(ret))
		return ret;

	return gpt_refresh();
}

EFI_STATUS gpt_create(UINTN start_lba, UINTN part_count, struct gpt_bin_part *gbp, logical_unit_t log_unit)
{
	EFI_STATUS ret;

	ret = gpt_cache_partition(log_unit);
	if (EFI_ERROR(ret))
		return ret;

	if (sdisk.partitions) {
		FreePool(sdisk.partitions);
		sdisk.partitions = NULL;
	}
	gpt_new(&sdisk.gpt_hd, start_lba, sdisk.bio->Media->BlockSize, sdisk.bio->Media->LastBlock);

	ret = gpt_check_partition_list(part_count, gbp);
	if (EFI_ERROR(ret))
		return ret;

	sdisk.partitions = gpt_fill_entries(part_count, gbp);
	sdisk.label_prefix_removed = FALSE;

	gpt_write_partition_tables();

	return EFI_SUCCESS;
}

EFI_STATUS gpt_get_partition_guid(CHAR16 *label, EFI_GUID *guid, logical_unit_t log_unit)
{
	EFI_STATUS ret;
	struct gpt_partition *part;

	ret = gpt_cache_partition(log_unit);
	if (EFI_ERROR(ret))
		return ret;

	part = gpt_find_partition(label);
	if (!part) {
		error(L"Failed to find '%s' partition", label);
		return EFI_NOT_FOUND;
	}

	CopyMem(guid, &part->unique, sizeof(*guid));

	return EFI_SUCCESS;
}

EFI_STATUS gpt_swap_partition(CHAR16 *label1, CHAR16 *label2, logical_unit_t log_unit)
{
	EFI_STATUS ret;
	struct gpt_partition *part1, *part2, save1;

	ret = gpt_cache_partition(log_unit);
	if (EFI_ERROR(ret))
		return ret;

	part1 = gpt_find_partition(label1);
	if (!part1) {
		error(L"Failed to find '%s' partition", label1);
		return EFI_NOT_FOUND;
	}

	part2 = gpt_find_partition(label2);
	if (!part2) {
		error(L"Failed to find '%s' partition", label2);
		return EFI_NOT_FOUND;
	}

	save1.starting_lba = part1->starting_lba;
	save1.ending_lba = part1->ending_lba;

	part1->starting_lba = part2->starting_lba;
	part1->ending_lba = part2->ending_lba;

	part2->starting_lba = save1.starting_lba;
	part2->ending_lba = save1.ending_lba;

	return gpt_write_partition_tables();
}