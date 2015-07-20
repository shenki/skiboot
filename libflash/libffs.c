/* Copyright 2013-2014 IBM Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifndef __SKIBOOT__
#include <sys/types.h>
#include <unistd.h>
#endif

#include <ccan/endian/endian.h>

#include "libffs.h"

enum ffs_type {
	ffs_type_flash,
	ffs_type_image,
};

struct ffs_handle {
	enum ffs_type		type;
	struct flash_chip	*chip;
	uint32_t		toc_offset;
	uint32_t		max_size;
	void			*cache;
	uint32_t		cached_size;
	struct blocklevel_device *bl;
	struct ffs_hdr		hdr;	/* Converted header */
};

static uint32_t ffs_checksum(void* data, size_t size)
{
	uint32_t i, csum = 0;

	for (i = csum = 0; i < (size/4); i++)
		csum ^= ((uint32_t *)data)[i];
	return csum;
}

static int ffs_check_convert_header(struct ffs_hdr *dst, struct ffs_hdr *src)
{
	dst->magic = be32_to_cpu(src->magic);
	if (dst->magic != FFS_MAGIC)
		return FFS_ERR_BAD_MAGIC;
	dst->version = be32_to_cpu(src->version);
	if (dst->version != FFS_VERSION_1)
		return FFS_ERR_BAD_VERSION;
	if (ffs_checksum(src, FFS_HDR_SIZE) != 0)
		return FFS_ERR_BAD_CKSUM;
	dst->size = be32_to_cpu(src->size);
	dst->entry_size = be32_to_cpu(src->entry_size);
	dst->entry_count = be32_to_cpu(src->entry_count);
	dst->block_size = be32_to_cpu(src->block_size);
	dst->block_count = be32_to_cpu(src->block_count);

	return 0;
}

int ffs_init(uint32_t offset, uint32_t max_size, struct blocklevel_device *bl,
		struct ffs_handle **ffs, int mark_ecc)
{
	struct ffs_hdr hdr;
	struct ffs_handle *f;
	uint32_t total_size;
	int rc, i;

	if (!ffs || !bl)
		return FLASH_ERR_PARM_ERROR;
	*ffs = NULL;

	rc = blocklevel_get_info(bl, NULL, &total_size, NULL);
	if (rc) {
		FL_ERR("FFS: Error %d retrieving flash info\n", rc);
		return rc;
	}
	if ((offset + max_size) < offset)
		return FLASH_ERR_PARM_ERROR;

	if ((max_size > total_size))
		return FLASH_ERR_PARM_ERROR;

	/* Read flash header */
	rc = blocklevel_read(bl, offset, &hdr, sizeof(hdr));
	if (rc) {
		FL_ERR("FFS: Error %d reading flash header\n", rc);
		return rc;
	}

	/* Allocate ffs_handle structure and start populating */
	f = malloc(sizeof(*f));
	if (!f)
		return FLASH_ERR_MALLOC_FAILED;
	memset(f, 0, sizeof(*f));

	f->toc_offset = offset;
	f->max_size = max_size;
	f->bl = bl;

	/* Convert and check flash header */
	rc = ffs_check_convert_header(&f->hdr, &hdr);
	if (rc) {
		FL_ERR("FFS: Error %d checking flash header\n", rc);
		goto out;
	}

	/*
	 * Decide how much of the image to grab to get the whole
	 * partition map.
	 */
	f->cached_size = f->hdr.block_size * f->hdr.size;
	FL_DBG("FFS: Partition map size: 0x%x\n", f->cached_size);

	/* Allocate cache */
	f->cache = malloc(f->cached_size);
	if (!f->cache) {
		rc = FLASH_ERR_MALLOC_FAILED;
		goto out;
	}

	/* Read the cached map */
	rc = blocklevel_read(bl, offset, f->cache, f->cached_size);
	if (rc) {
		FL_ERR("FFS: Error %d reading flash partition map\n", rc);
		goto out;
	}

	if (mark_ecc) {
		uint32_t start, total_size;
		bool ecc;
		for (i = 0; i < f->hdr.entry_count; i++) {
			ffs_part_info(f, i, NULL, &start, &total_size, NULL, &ecc);
			if (ecc) {
				rc = blocklevel_ecc_protect(bl, start, total_size);
				if (rc) {
					FL_ERR("Failed to blocklevel_ecc_protect(0x%08x, 0x%08x)\n",
					       start, total_size);
					goto out;
				}
			}  /* ecc */
		} /* for */
	}

out:
	if (rc == 0)
		*ffs = f;
	else
		free(f);

	return rc;
}

/* ffs_open_image is Linux only as it uses lseek, which skiboot does not
 * implement */
#ifndef __SKIBOOT__
int ffs_open_image(int fd, uint32_t size, uint32_t toc_offset,
		   struct ffs_handle **ffsh)
{
	struct ffs_hdr hdr;
	struct ffs_handle *f;
	int rc;

	if (!ffsh)
		return FLASH_ERR_PARM_ERROR;
	*ffsh = NULL;

	if (fd < 0)
		return FLASH_ERR_PARM_ERROR;

	if ((toc_offset + size) < toc_offset)
		return FLASH_ERR_PARM_ERROR;

	/* Read flash header */
	rc = lseek(fd, toc_offset, SEEK_SET);
	if (rc < 0)
		return FLASH_ERR_PARM_ERROR;

	rc = read(fd, &hdr, sizeof(hdr));
	if (rc != sizeof(hdr))
		return FLASH_ERR_BAD_READ;

	/* Allocate ffs_handle structure and start populating */
	f = malloc(sizeof(*f));
	if (!f)
		return FLASH_ERR_MALLOC_FAILED;
	memset(f, 0, sizeof(*f));
	f->type = ffs_type_image;
	f->toc_offset = toc_offset;
	f->max_size = size;
	f->chip = NULL;

	/* Convert and check flash header */
	rc = ffs_check_convert_header(&f->hdr, &hdr);
	if (rc) {
		FL_ERR("FFS: Error %d checking flash header\n", rc);
		free(f);
		return rc;
	}

	/*
	 * Decide how much of the image to grab to get the whole
	 * partition map.
	 */
	f->cached_size = f->hdr.block_size * f->hdr.size;
	FL_DBG("FFS: Partition map size: 0x%x\n", f->cached_size);

	/* Allocate cache */
	f->cache = malloc(f->cached_size);
	if (!f->cache) {
		free(f);
		return FLASH_ERR_MALLOC_FAILED;
	}

	/* Read the cached map */
	rc = lseek(fd, toc_offset, SEEK_SET);
	if (rc < 0)
		return FLASH_ERR_PARM_ERROR;

	rc = read(fd, f->cache, f->cached_size);
	if (rc != f->cached_size) {
		FL_ERR("FFS: Error %d reading flash partition map\n", rc);
		free(f);
		return FLASH_ERR_BAD_READ;
	}

	*ffsh = f;

	return 0;
}
#endif /*!__SKIBOOT__*/

void ffs_close(struct ffs_handle *ffs)
{
	if (ffs->cache)
		free(ffs->cache);
	free(ffs);
}

static struct ffs_entry *ffs_get_part(struct ffs_handle *ffs, uint32_t index,
				      uint32_t *out_offset)
{
	uint32_t esize = ffs->hdr.entry_size;
	uint32_t offset = FFS_HDR_SIZE + index * esize;

	if (index > ffs->hdr.entry_count)
		return NULL;
	if (out_offset)
		*out_offset = ffs->toc_offset + offset;
	return (struct ffs_entry *)(ffs->cache + offset);
}

static int ffs_check_convert_entry(struct ffs_entry *dst, struct ffs_entry *src)
{
	if (ffs_checksum(src, FFS_ENTRY_SIZE) != 0)
		return FFS_ERR_BAD_CKSUM;
	memcpy(dst->name, src->name, sizeof(dst->name));
	dst->base = be32_to_cpu(src->base);
	dst->size = be32_to_cpu(src->size);
	dst->pid = be32_to_cpu(src->pid);
	dst->id = be32_to_cpu(src->id);
	dst->type = be32_to_cpu(src->type);
	dst->flags = be32_to_cpu(src->flags);
	dst->actual = be32_to_cpu(src->actual);
	dst->user.datainteg = be16_to_cpu(src->user.datainteg);

	return 0;
}

int ffs_lookup_part(struct ffs_handle *ffs, const char *name,
		    uint32_t *part_idx)
{
	struct ffs_entry ent;
	uint32_t i;
	int rc;

	/* Lookup the requested partition */
	for (i = 0; i < ffs->hdr.entry_count; i++) {
		struct ffs_entry *src_ent  = ffs_get_part(ffs, i, NULL);
		rc = ffs_check_convert_entry(&ent, src_ent);
		if (rc) {
			FL_ERR("FFS: Bad entry %d in partition map\n", i);
			continue;
		}
		if (!strncmp(name, ent.name, sizeof(ent.name)))
			break;
	}
	if (i >= ffs->hdr.entry_count)
		return FFS_ERR_PART_NOT_FOUND;
	if (part_idx)
		*part_idx = i;
	return 0;
}

int ffs_part_info(struct ffs_handle *ffs, uint32_t part_idx,
		  char **name, uint32_t *start,
		  uint32_t *total_size, uint32_t *act_size, bool *ecc)
{
	struct ffs_entry *raw_ent;
	struct ffs_entry ent;
	char *n;
	int rc;

	if (part_idx >= ffs->hdr.entry_count)
		return FFS_ERR_PART_NOT_FOUND;

	raw_ent = ffs_get_part(ffs, part_idx, NULL);
	if (!raw_ent)
		return FFS_ERR_PART_NOT_FOUND;

	rc = ffs_check_convert_entry(&ent, raw_ent);
	if (rc) {
		FL_ERR("FFS: Bad entry %d in partition map\n", part_idx);
		return rc;
	}
	if (start)
		*start = ent.base * ffs->hdr.block_size;
	if (total_size)
		*total_size = ent.size * ffs->hdr.block_size;
	if (act_size)
		*act_size = ent.actual;
	if (ecc)
		*ecc = ((ent.user.datainteg & FFS_ENRY_INTEG_ECC) != 0);

	if (name) {
		n = malloc(PART_NAME_MAX + 1);
		memset(n, 0, PART_NAME_MAX + 1);
		strncpy(n, ent.name, PART_NAME_MAX);
		*name = n;
	}
	return 0;
}

int ffs_update_act_size(struct ffs_handle *ffs, uint32_t part_idx,
			uint32_t act_size)
{
	struct ffs_entry *ent;
	uint32_t offset;

	if (part_idx >= ffs->hdr.entry_count) {
		FL_DBG("FFS: Entry out of bound\n");
		return FFS_ERR_PART_NOT_FOUND;
	}

	ent = ffs_get_part(ffs, part_idx, &offset);
	if (!ent) {
		FL_DBG("FFS: Entry not found\n");
		return FFS_ERR_PART_NOT_FOUND;
	}
	FL_DBG("FFS: part index %d at offset 0x%08x\n",
	       part_idx, offset);

	/*
	 * NOTE: We are accessing the unconverted ffs_entry from the PNOR here
	 * (since we are going to write it back) so we need to be endian safe.
	 */
	if (ent->actual == cpu_to_be32(act_size)) {
		FL_DBG("FFS: ent->actual alrady matches: 0x%08x==0x%08x\n",
		       cpu_to_be32(act_size), ent->actual);
		return 0;
	}
	ent->actual = cpu_to_be32(act_size);
	ent->checksum = ffs_checksum(ent, FFS_ENTRY_SIZE_CSUM);
	if (!ffs->chip)
		return 0;

	return blocklevel_write(ffs->bl, offset, ent, FFS_ENTRY_SIZE);
}

