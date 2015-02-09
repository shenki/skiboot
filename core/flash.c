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

#include <skiboot.h>
#include <lock.h>
#include <opal.h>
#include <opal-msg.h>
#include <opal-api.h>
#include <device.h>
#include <libflash/libflash.h>
#include <libflash/libffs.h>

struct flash {
	bool			registered;
	struct flash_chip	*chip;
	uint32_t		size;
	uint32_t		block_size;
};

#define MAX_FLASH 1
static struct flash flashes[MAX_FLASH];
static struct flash *system_flash;

/* Using a single lock as we only have one flash at present. */
static struct lock flash_lock;

static void flash_add_dt_partition_node(struct dt_node *flash_node, char *name,
		uint32_t start, uint32_t size)
{
	struct dt_node *part_node;

	part_node = dt_new_addr(flash_node, "partition", start);
	dt_add_property_cells(part_node, "reg", start, size);
	if (name && strlen(name))
		dt_add_property_strings(part_node, "label", name);
}

static void flash_add_dt_node(struct flash *flash, int id,
		struct ffs_handle *ffs)
{
	struct dt_node *flash_node;
	int i;

	flash_node = dt_new_addr(opal_node, "flash", id);
	dt_add_property_strings(flash_node, "compatible", "ibm,opal-flash");
	dt_add_property_cells(flash_node, "ibm,opal-id", id);
	dt_add_property_cells(flash_node, "reg", 0, flash->size);
	dt_add_property_cells(flash_node, "ibm,flash-block-size",
			flash->block_size);

	/* we fix to 32-bits */
	dt_add_property_cells(flash_node, "#address-cells", 1);
	dt_add_property_cells(flash_node, "#size-cells", 1);

	if (!ffs)
		return;

	for (i = 0; ; i++) {
		uint32_t start, size;
		char *name;
		int rc;

		rc = ffs_part_info(ffs, i, &name, &start, NULL, &size);
		if (rc)
			break;

		flash_add_dt_partition_node(flash_node, name, start, size);
	}
}

int flash_register(struct flash_chip *chip, bool is_system_flash)
{
	uint32_t size, block_size;
	struct ffs_handle *ffs;
	struct flash *flash;
	const char *name;
	unsigned int i;
	int rc;

	rc = flash_get_info(chip, &name, &size, &block_size);
	if (rc)
		return rc;

	prlog(PR_INFO, "FLASH: registering flash device %s "
			"(size 0x%x, blocksize 0x%x)\n",
			name ?: "(unnamed)", size, block_size);

	lock(&flash_lock);
	for (i = 0; i < ARRAY_SIZE(flashes); i++) {
		if (flashes[i].registered)
			continue;

		flash = &flashes[i];
		flash->registered = true;
		flash->chip = chip;
		flash->size = size;
		flash->block_size = block_size;
		break;
	}

	if (!flash) {
		unlock(&flash_lock);
		prlog(PR_ERR, "FLASH: No flash slots available\n");
		return OPAL_RESOURCE;
	}

	rc = ffs_open_flash(chip, 0, flash->size, &ffs);
	if (rc) {
		prlog(PR_WARNING, "FLASH: No ffs info; "
				"using raw device only\n");
		ffs = NULL;
	}

	if (is_system_flash && !system_flash)
		system_flash = flash;

	flash_add_dt_node(flash, i, ffs);

	ffs_close(ffs);

	unlock(&flash_lock);

	return OPAL_SUCCESS;
}

enum flash_op {
	FLASH_OP_READ,
	FLASH_OP_WRITE,
	FLASH_OP_ERASE,
};

static int64_t opal_flash_op(uint64_t id, uint64_t offset, uint64_t buf,
		uint64_t size, uint64_t token, enum flash_op op)
{
	struct flash *flash;
	uint32_t mask;
	int rc;

	if (id >= ARRAY_SIZE(flashes))
		return OPAL_PARAMETER;

	if (!try_lock(&flash_lock))
		return OPAL_BUSY;

	flash = &flashes[id];
	if (!flash->registered) {
		rc = OPAL_PARAMETER;
		goto err;
	}

	if (size >= flash->size || offset >= flash->size
			|| offset + size >= flash->size) {
		rc = OPAL_PARAMETER;
		goto err;
	}

	mask = flash->block_size - 1;
	if (size & mask || offset & mask) {
		rc = OPAL_PARAMETER;
		goto err;
	}

	switch (op) {
	case FLASH_OP_READ:
		rc = flash_read(flash->chip, offset, (void *)buf, size);
		break;
	case FLASH_OP_WRITE:
		rc = flash_write(flash->chip, offset, (void *)buf, size, false);
		break;
	case FLASH_OP_ERASE:
		rc = flash_erase(flash->chip, offset, size);
		break;
	default:
		assert(0);
	}

	unlock(&flash_lock);

	opal_queue_msg(OPAL_MSG_ASYNC_COMP, NULL, NULL, token, rc);
	return OPAL_ASYNC_COMPLETION;

err:
	unlock(&flash_lock);
	return rc;
}

static int64_t opal_flash_read(uint64_t id, uint64_t offset, uint64_t buf,
		uint64_t size, uint64_t token)
{
	return opal_flash_op(FLASH_OP_READ, id, offset, buf, size, token);
}

static int64_t opal_flash_write(uint64_t id, uint64_t offset, uint64_t buf,
		uint64_t size, uint64_t token)
{
	return opal_flash_op(FLASH_OP_WRITE, id, offset, buf, size, token);
}

static int64_t opal_flash_erase(uint64_t id, uint64_t offset, uint64_t size,
		uint64_t token)
{
	return opal_flash_op(FLASH_OP_ERASE, id, offset, 0L, size, token);
}

opal_call(OPAL_FLASH_READ, opal_flash_read, 5);
opal_call(OPAL_FLASH_WRITE, opal_flash_write, 5);
opal_call(OPAL_FLASH_ERASE, opal_flash_erase, 4);
