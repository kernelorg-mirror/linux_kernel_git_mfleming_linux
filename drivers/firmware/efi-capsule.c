/*
 * EFI Update Capsule support.
 *
 * Copyright 2013 Intel Corporation <matt.fleming@intel.com>
 *
 * Note that the functions in this file that interact with the
 * firmware probably need to grow a new EFI subsystem lock because
 * there are certain runtime functions that MUST NOT be invoked at the
 * same time on different processors.
 *
 * This file is part of the Linux kernel, and is made available under
 * the terms of the GNU General Public License version 2.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/debugfs.h>
#include <linux/efi.h>
#include <linux/firmware.h>
#include <linux/pstore.h>
#include <linux/module.h>

typedef struct {
	u64 length;
	union {
		u64 data;
		u64 ptr;
	};
} efi_capsule_block_desc_t;

struct efi_capsule_ctx {
	void *capsule;
	size_t capsule_size;
	void *data;
	size_t data_size;
};

struct efi_capsule_pstore_buf {
	void *buf;
	size_t size;
	atomic_long_t offset;
};

struct efi_capsule_pstore {
	/* Previous records */
	efi_capsule_header_t **hdrs;
	uint32_t hdrs_num;
	off_t hdr_offset;	/* Offset into current header */

	/* New records */
	struct efi_capsule_pstore_buf console;
	struct efi_capsule_pstore_buf ftrace;
	struct efi_capsule_pstore_buf dmesg;
};

struct efi_capsule_pstore_record {
	u64 timestamp;
	u64 id;
	enum pstore_type_id type;
	size_t size;
	char data[];
} __packed;

static struct pstore_info efi_capsule_info;

static int efi_reset_type;
static u64 efi_capsule_max_size;

/*
 * Information about capsules we pulled from the EFI System Table.
 */
static efi_capsule_header_t **prev_capsules;
static u32 efi_capsule_num;

/**
 * efi_capsule_build - alloc data buffer and fill out the header
 * @guid: vendor's guid
 * @data_size: size in bytes of the capsule data
 *
 * This is a helper function for allocating enough room for user data
 * + the size of an EFI capsule header.
 *
 * Returns a pointer to an allocated capsule on success, an ERR_PTR()
 * value on error.
 */
static struct efi_capsule_ctx *
efi_capsule_build(efi_guid_t guid, size_t data_size)
{
	efi_capsule_header_t *capsule;
	struct efi_capsule_ctx *ctx;
	size_t capsule_size;
	void *buf;

	capsule_size = data_size + sizeof(*capsule);
	if (capsule_size > efi_capsule_max_size)
		return ERR_PTR(-ENOSPC);

	ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		pr_err("failed to allocate capsule context memory\n");
		return ERR_PTR(-ENOMEM);
	}

	pr_info("allocating: %zu\n", capsule_size);
	buf = kzalloc(capsule_size, GFP_KERNEL);
	if (!buf) {
		kfree(ctx);
		return ERR_PTR(-ENOMEM);
	}

	ctx->capsule = buf;
	ctx->capsule_size = capsule_size;
	ctx->data = buf + sizeof(*capsule);
	ctx->data_size = data_size;

	pr_info("allocated %zd bytes of capsule memory\n", data_size);

	/*
	 * Setup the EFI capsule header.
	 */
	capsule = (efi_capsule_header_t *)ctx->capsule;
	memcpy(&capsule->guid, &guid, sizeof(guid));

	capsule->flags = EFI_CAPSULE_PERSIST_ACROSS_RESET |
		EFI_CAPSULE_POPULATE_SYSTEM_TABLE;

	capsule->headersize = sizeof(*capsule);
	capsule->imagesize = capsule_size;

	return ctx;
}

/**
 * efi_update_capsule - pass a capsule to the firmware.
 * @capsule: capsule to send to the firmware.
 * @size: size of the capsule, including the capsule header.
 *
 * Map @capsule with EFI capsule block descriptors in PAGE_SIZE chunks.
 * @size needn't necessarily be a multiple of PAGE_SIZE - we can
 * handle a trailing chunk that is smaller than PAGE_SIZE.
 *
 * It is assumed that an EFI capsule header is at the beginning of
 * @capsule, with capsule data following it. Therefore @size includes
 * both the capsule header and capsule data.
 *
 * LOCKING: Beware of which runtime services we need to serialize
 * with, e.g. ourselves plus any of the variable functions.
 *
 * Return 0 on success.
 */
static int efi_update_capsule(void *capsule, size_t size)
{
	efi_capsule_block_desc_t *blocks;
	efi_capsule_header_t *capsules;
	unsigned long address = (unsigned long)capsule;
	efi_status_t status;
	unsigned int nblocks;
	int i;

	nblocks = DIV_ROUND_UP(size, PAGE_SIZE);

	/*
	 * We also need space for the end descriptor that marks the
	 * end of the block desc chain.
	 */
	nblocks++;

	/*
	 * A current limitation is that we don't support more than,
	 *
	 *	(PAGE_SIZE / efi_capsule_block_desc_t)
	 *
	 * entries because that would require to use the continuation
	 * pointer field in the block desc, because any pages that we
	 * allocate with kzalloc() will not necessarily be physically
	 * contiguous.
	 */
	if (nblocks * sizeof(*blocks) > PAGE_SIZE) {
		pr_err("capsule requires too many block descs: %d * %lu > %lu\n",
		       nblocks, sizeof(*blocks), PAGE_SIZE);
		return -EINVAL;
	}

	blocks = kzalloc(nblocks * sizeof(*blocks), GFP_KERNEL);
	if (!blocks)
		return -ENOMEM;

	for (i = 0; i < nblocks - 1; i++) {
		int sz = min(size, PAGE_SIZE);

		blocks[i].length = sz;
		blocks[i].data = __pa(address);

		address += PAGE_SIZE;
		size -= sz;
	}

	if (size)
		pr_err("size left over: %zu\n", size);

	capsules = (efi_capsule_header_t *)capsule;
	status = efi.update_capsule(&capsules, 1, __pa(blocks));
	if (status != EFI_SUCCESS) {
		pr_err("update_capsule fail: 0x%lx\n", status);
		return -EINVAL;	/* XXX bogus error code */
	}

	return 0;
}

/**
 * efi_capsule_lookup - search capsule array for entries.
 * @guid: the guid to search for.
 * @nr_caps: the number of entries found.
 *
 * Map each capsule header into the kernel's virtual address space and
 * inspect the guid. Build an array of capsule headers with every
 * capsule that is found with @guid. If a match is found the capsule
 * remains mapped, otherwise it is unmapped.
 *
 * Returns an array of capsule headers, each element of which has the
 * guid @guid. The number of elements in the array is stored in
 * @nr_caps. Returns %NULL if no capsules were found and stores zero
 * in @nr_caps.
 */
static efi_capsule_header_t **
efi_capsule_lookup(efi_guid_t guid, uint32_t *nr_caps)
{
	efi_capsule_header_t **capsules = NULL;
	size_t capsules_size = 0;
	int i;

	*nr_caps = 0;
	for (i = 0; i < efi_capsule_num; i++) {
		efi_capsule_header_t *c;
		size_t size;

		c = ioremap((resource_size_t)prev_capsules[i], sizeof(*c));
		if (!c) {
			pr_err("failed to ioremap capsule\n");
			continue;
		}

		size = c->imagesize;
		iounmap(c);

		c = ioremap((resource_size_t)prev_capsules[i], size);
		if (!c) {
			pr_err("failed to ioremap header + data\n");
			continue;
		}

		if (!efi_guidcmp(c->guid, guid)) {
			capsules_size += sizeof(**capsules);
			capsules = krealloc(capsules, capsules_size, GFP_KERNEL);
			if (!capsules)
				return ERR_PTR(-ENOMEM);

			capsules[(*nr_caps)++] = c;
			continue;
		}

		iounmap(c);
	}

	return capsules;
}

/*
 * We may not be in a position to allocate memory at the time of a
 * crash, so pre-allocate some space now and register it with the
 * firmware via efi_capsule_update().
 *
 * Also, iterate through the array of capsules pointed to from the EFI
 * system table and take note of any LINUX_EFI_CRASH_GUID
 * capsules. They will be parsed by efi_capsule_pstore_read().
 */
static int efi_capsule_pstore_setup(void)
{
	struct efi_capsule_pstore_record *rec;
	struct efi_capsule_pstore *pctx = NULL;
	struct efi_capsule_ctx *console_ctx = NULL;
	struct efi_capsule_ctx *ftrace_ctx = NULL;
	struct efi_capsule_ctx *dmesg_ctx = NULL;
	efi_capsule_header_t **hdrs;
	uint32_t hdrs_num;
	void *crash_buf = NULL;
	size_t size, crash_size;
	int rv;

	pctx = kzalloc(sizeof(*pctx), GFP_KERNEL);
	if (!pctx)
		return -ENOMEM;

	size = 16 * 1024;
	if (size > efi_capsule_max_size) {
		size = efi_capsule_max_size;
		WARN_ON_ONCE(1);
	}

	/* Allocate all the capsules upfront */
	dmesg_ctx = efi_capsule_build(LINUX_EFI_CRASH_GUID, size);
	if (IS_ERR(dmesg_ctx)) {
		rv = PTR_ERR(dmesg_ctx);
		dmesg_ctx = NULL;
		goto fail;
	}

	ftrace_ctx = efi_capsule_build(LINUX_EFI_CRASH_GUID, size);
	if (IS_ERR(ftrace_ctx)) {
		rv = PTR_ERR(ftrace_ctx);
		ftrace_ctx = NULL;
		goto fail;
	}

	console_ctx = efi_capsule_build(LINUX_EFI_CRASH_GUID, size);
	if (IS_ERR(console_ctx)) {
		rv = PTR_ERR(console_ctx);
		console_ctx = NULL;
		goto fail;
	}

	crash_size = 4096;
	crash_buf = kmalloc(crash_size, GFP_KERNEL);
	if (!crash_buf) {
		rv = -ENOMEM;
		goto fail;
	}

	/* Register with the firmware. */
	rv = efi_update_capsule(dmesg_ctx->capsule, dmesg_ctx->capsule_size);
	if (rv)
		goto fail;

	rv = efi_update_capsule(ftrace_ctx->capsule, ftrace_ctx->capsule_size);
	if (rv)
		goto fail_ftrace;

	rv = efi_update_capsule(console_ctx->capsule, console_ctx->capsule_size);
	if (rv)
		goto fail_console;

	pctx->dmesg.size = dmesg_ctx->data_size;
	pctx->dmesg.buf = dmesg_ctx->data;
	atomic_long_set(&pctx->dmesg.offset, 0);

	/*
	 * Setup the pstore records for the ring-buffers.
	 */
	pctx->ftrace.size = ftrace_ctx->data_size - offsetof(typeof(*rec), data);
	pctx->ftrace.buf = ftrace_ctx->data + offsetof(typeof(*rec), data);
	atomic_long_set(&pctx->ftrace.offset, 0);
	rec = ftrace_ctx->data;
	rec->type = PSTORE_TYPE_FTRACE;
	rec->size = pctx->ftrace.size;

	pctx->console.size = console_ctx->data_size - offsetof(typeof(*rec), data);
	pctx->console.buf = console_ctx->data + offsetof(typeof(*rec), data);
	atomic_long_set(&pctx->console.offset, 0);
	rec = console_ctx->data;
	rec->type = PSTORE_TYPE_CONSOLE;
	rec->size = pctx->console.size;

	/*
	 * Read any pstore entries that were passed across a reboot.
	 */
	hdrs = efi_capsule_lookup(LINUX_EFI_CRASH_GUID, &hdrs_num);
	pctx->hdrs_num = hdrs_num;
	pctx->hdrs = IS_ERR(hdrs) ? NULL : hdrs;

	if (pctx->hdrs_num)
		pr_info("found Linux Crash Capsule\n");

	/*
	 * Register the capsule backend with pstore.
	 */
	spin_lock_init(&efi_capsule_info.buf_lock);

	efi_capsule_info.buf = crash_buf;
	efi_capsule_info.bufsize = crash_size;
	efi_capsule_info.data = pctx;

	rv = pstore_register(&efi_capsule_info);
	if (rv)
		pr_err("capsule support registration failed for pstore: %d\n", rv);

	return rv;

fail:
	kfree(dmesg_ctx);
fail_ftrace:
	kfree(ftrace_ctx);
fail_console:
	kfree(console_ctx);

	kfree(crash_buf);
	kfree(pctx);
	return rv;
}

/*
 * Return the next pstore record that was passed to us across a reboot
 * in an EFI capsule.
 *
 * This is expected to be called under the pstore
 * read_mutex. Therefore, no serialisation is done here.
 */
static struct efi_capsule_pstore_record *
get_pstore_read_record(struct efi_capsule_pstore *pctx)
{
	struct efi_capsule_pstore_record *rec;
	efi_capsule_header_t *hdr;
	off_t remaining;

next:
	if (!pctx->hdrs_num)
		return NULL;

	hdr = pctx->hdrs[pctx->hdrs_num - 1];
	rec = (void *)hdr + hdr->headersize + pctx->hdr_offset;

	remaining = hdr->imagesize - hdr->headersize - pctx->hdr_offset - offsetof(typeof(*rec), data);

	/*
	 * A single EFI capsule may contain multiple pstore
	 * records. It may also only be partially filled with pstore
	 * records, which we can detect by checking for a record with
	 * zero size.
	 *
	 * If there are no more entries in this capsule try the next.
	 */
	if (!rec->size) {
		pctx->hdrs_num--;
		pctx->hdr_offset = 0;
		goto next;
	}

	/*
	 * If we've finished parsing all records in this capsule, move
	 * onto the next. Otherwise, increment the offset into the
	 * current capsule (pctx->hdr_offset).
	 */
	if (rec->size == remaining) {
		pctx->hdrs_num--;
		pctx->hdr_offset = 0;
	} else
		pctx->hdr_offset += rec->size + offsetof(typeof(*rec), data);

	return rec;
}

static ssize_t efi_capsule_pstore_read(u64 *id, enum pstore_type_id *type,
				       int *count, struct timespec *time,
				       char **buf, struct pstore_info *psi)
{
	struct efi_capsule_pstore_record *rec;
	struct efi_capsule_pstore *pctx = psi->data;
	ssize_t size;

	rec = get_pstore_read_record(pctx);
	if (!rec)
		return 0;

	*type = rec->type;
	time->tv_sec = rec->timestamp;
	time->tv_nsec = 0;
	size = rec->size;
	*id = rec->id;

	*buf = kmalloc(size, GFP_KERNEL);
	if (!*buf)
		return -ENOMEM;

	memcpy(*buf, rec->data, size);

	return size;
}

/*
 * We expect to be called with ->buf_lock held, and so don't perform
 * any serialisation.
 */
static struct notrace efi_capsule_pstore_record *
get_pstore_write_record(struct efi_capsule_pstore_buf *pbuf, size_t *size)
{
	struct efi_capsule_pstore_record *rec;
	long offset = atomic_long_read(&pbuf->offset);

	if (offset == pbuf->size)
		return NULL;

	/* Trim 'size' if there isn't enough remaining space */
	if (offset + *size > pbuf->size)
		*size -= (pbuf->size - offset);

	rec = pbuf->buf + offset;
	atomic_long_add(offsetof(typeof(*rec), data) + *size, &pbuf->offset);

	return rec;
}

static int notrace
efi_capsule_pstore_write(enum pstore_type_id type,
			 enum kmsg_dump_reason reason, u64 *id,
			 unsigned int part, int count, size_t size,
			 struct pstore_info *psi)
{
	struct efi_capsule_pstore_record *rec;
	struct efi_capsule_pstore *pctx = psi->data;

	/*
	 * A zero size record would break our detection of
	 * partially-filled capsules.
	 */
	if (!size)
		return -EINVAL;

	rec = get_pstore_write_record(&pctx->dmesg, &size);
	if (!rec)
		return -ENOSPC;

	rec->type = type;
	rec->timestamp = get_seconds();
	rec->size = size;
	rec->id = (*id)++;
	memcpy(rec->data, psi->buf, size);

	efi.reset_system(efi_reset_type, EFI_SUCCESS, 0, NULL);

	return 0;
}

static notrace void *
get_pstore_buf(struct efi_capsule_pstore_buf *pbuf, size_t size)
{
	long next, curr;

	if (size > pbuf->size)
		return NULL;

	do {
		curr = atomic_long_read(&pbuf->offset);
		next = curr + size;

		/* Wrap? */
		if (next > pbuf->size) {
			next = size;
			if (atomic_long_cmpxchg(&pbuf->offset, curr, next)) {
				curr = 0;
				break;
			}

			continue;
		}

	} while (atomic_long_cmpxchg(&pbuf->offset, curr, next) != curr);

	return pbuf->buf + curr;
}

static int notrace
efi_capsule_pstore_write_buf(enum pstore_type_id type,
			     enum kmsg_dump_reason reason,
			     u64 *id, unsigned int part,
			     const char *buf, size_t size,
			     struct pstore_info *psi)
{
	struct efi_capsule_pstore *pctx = psi->data;
	void *dst;

	if (type == PSTORE_TYPE_FTRACE)
		dst = get_pstore_buf(&pctx->ftrace, size);
	else if (type == PSTORE_TYPE_CONSOLE)
		dst = get_pstore_buf(&pctx->console, size);
	else
		return -EINVAL;

	if (!dst)
		return -ENOSPC;

	memcpy(dst, buf, size);
	return 0;
}

static struct pstore_info efi_capsule_info = {
	.owner     = THIS_MODULE,
	.name      = "efi-capsule",
	.read      = efi_capsule_pstore_read,
	.write     = efi_capsule_pstore_write,
	.write_buf = efi_capsule_pstore_write_buf,
};

/*
 * Construct a fake capsule header to query capsule support.
 */
static int __check_capsule_support(void)
{
	efi_capsule_header_t *capsule;
	efi_status_t status;
	efi_guid_t guid = LINUX_EFI_CRASH_GUID;
	u64 max;
	int rv = 0;

	capsule = kmalloc(sizeof(*capsule), GFP_KERNEL);
	if (!capsule)
		return -ENOMEM;

	capsule->imagesize = capsule->headersize = sizeof(*capsule);
	capsule->flags = EFI_CAPSULE_PERSIST_ACROSS_RESET |
		EFI_CAPSULE_POPULATE_SYSTEM_TABLE;
	memcpy(&capsule->guid, &guid, sizeof(guid));

	status = efi.query_capsule_caps(&capsule, 1, &max, &efi_reset_type);
	if (status != EFI_SUCCESS) {
		rv = -ENODEV;
		goto out;
	}

	switch (efi_reset_type) {
	case EFI_RESET_COLD:
	case EFI_RESET_WARM:
	case EFI_RESET_SHUTDOWN:
		rv = 0;
		break;
	default:
		rv = -EINVAL;
		goto out;
	}

	efi_capsule_max_size = max;

out:
	kfree(capsule);
	return rv;
}

/**
 * efi_capsule_init - initialise the EFI capsule system
 *
 * Check whether the firmware supports EFI capsules, read in any
 * capsules that are exported in the EFI configuration tables and
 * register the capsule backend with pstore.
 */
static int __init efi_capsule_init(void)
{
	void *capsule;
	int rv;
	const char *reset_str[] = {
		"cold",
		"warm",
		"shutdown",
	};

	rv = __check_capsule_support();
	if (rv)
		return rv;

	pr_info("EFI Capsule support enabled, reset type: ");
	pr_info("%s, ", reset_str[efi_reset_type]);
	pr_info("max capsule size: %llu\n", efi_capsule_max_size);

	if (efi.capsule != EFI_INVALID_TABLE_ADDR) {
		capsule = ioremap(efi.capsule, sizeof(efi_capsule_num));
		if (!capsule)
			return -ENOMEM;

		/*
		 * The array of capsules is prefixed with the number of
		 * capsule entries in the array.
		 */
		efi_capsule_num = *(uint32_t *)capsule;
		iounmap(capsule);

		if (efi_capsule_num) {
			size_t size = efi_capsule_num * sizeof(*capsule);

			capsule = ioremap(efi.capsule, size);
			if (!capsule)
				return -ENOMEM;

			capsule += sizeof(uint32_t *);
			prev_capsules = (efi_capsule_header_t **)capsule;
			if (!*prev_capsules)
				pr_err("capsule array has no entries\n");
		}
	}

	efi_capsule_pstore_setup();

	return rv;
}
device_initcall(efi_capsule_init);
